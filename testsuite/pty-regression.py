# POSIX interaction regression; uses the standard library and a C compiler.
import os, sys, pty, select, time, signal, termios, fcntl, struct, re, subprocess
from pathlib import Path
import tempfile
if len(sys.argv) != 3:
    raise SystemExit("Usage: python3 pty-regression.py /path/to/cgdb /path/to/gdb")
cgdb = str(Path(sys.argv[1]).resolve())
gdb = str(Path(sys.argv[2]).resolve())
workspace = tempfile.TemporaryDirectory(prefix='cgdb-pty-test-')
root = Path(workspace.name)
fixture = root/'fixture.c'
fixture.write_text('#include <stdio.h>\n#include <unistd.h>\nint main(void) {\n puts("FIRST_OUTPUT");\n fprintf(stdout,"SECOND_OUTPUT\\n");\n puts("SPIN_READY");\n while(1) usleep(10000);\n}\n')
subprocess.run([os.environ.get('CC', 'cc'),'-g','-O0',str(fixture),'-o',str(root/'fixture')],check=True)
env = dict(os.environ, TERM=os.environ.get('TERM', 'xterm-256color'), HOME=str(root/'home'), LC_ALL='C', DEBUGINFOD_URLS='')
env.pop('CGDB_DIR', None)
shell_probe = root/'shell-probe'
shell_probe.write_text("#!/bin/sh\nprintf 'CGDB_TEST_SHELL_READY\\n'\nexec /bin/sh\n")
shell_probe.chmod(0o755)
env['SHELL'] = str(shell_probe)
Path(env['HOME']).mkdir(exist_ok=True)
master, slave = pty.openpty()
fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40,120,0,0))
before = termios.tcgetattr(slave)
pid = os.fork()
if pid == 0:
    os.setsid()
    fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
    for fd in (0,1,2): os.dup2(slave,fd)
    os.close(master); os.close(slave)
    os.chdir(root)
    args = [cgdb, '-d', gdb, '-nx', '-q']
    if env.get('GDB_DATA_DIRECTORY'):
        args.append('--data-directory=' + env['GDB_DATA_DIRECTORY'])
    os.execve(cgdb, args + [str(root/'fixture')], env)
log = open(os.environ.get('CGDB_TEST_LOG', str(root/'terminal.log')), 'wb')
pending = b''
def clean(b):
    return re.sub(rb'\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07)|[()][0-9A-Za-z]|[=>78])',b'',b).replace(b'\r',b'').replace(b'\x0e',b'').replace(b'\x0f',b'')
def need(text, timeout=12):
    global pending
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        decoded=clean(pending)
        if text.encode() in decoded:
            pending=decoded.split(text.encode(), 1)[1]
            return
        if select.select([master],[],[],0.1)[0]:
            data=os.read(master,65536)
            if not data: break
            log.write(data); log.flush(); pending+=data
    raise AssertionError('Timeout: '+repr(text)+'; tail='+repr(clean(pending)[-1500:]))
def send(s):
    # A curses repaint can repeat old prompts. Drain it before the next input,
    # so assertions cannot consume a prompt from the previous command.
    global pending
    deadline = time.monotonic() + 1
    while time.monotonic() < deadline and select.select([master], [], [], .1)[0]:
        data = os.read(master, 65536)
        if not data: break
        log.write(data); log.flush()
    pending = b''
    os.write(master, s.encode())
def command(s):
    send(s+'\r'); need('(gdb)')
try:
    need('(gdb)')
    command('set confirm off'); command('set pagination off')
    command('set startup-with-shell off')
    command('break main'); command('run')
    command('next')
    send('next\r'); need('SECOND_OUTPUT'); need('(gdb)')
    print('PASS: Linux GDB startup, breakpoint, stepping, stdout',flush=True)
    send('\x1b'); time.sleep(.15); send('o'); need('Select a file'); send('q')
    time.sleep(.15); send(':shell\r'); need('CGDB_TEST_SHELL_READY')
    send("printf 'SHELL_%s_OK\\n' REVIEW\r"); need('SHELL_REVIEW_OK')
    send('exit\r'); need('Hit ENTER to continue...')
    time.sleep(.3); send('\r'); time.sleep(.3); send('i')
    send('print 813000+57\r'); need('813057')
    print('PASS: source dialog, shell return and live debugger',flush=True)
    for rows,cols in [(18,61),(47,213),(40,120)]:
        fcntl.ioctl(slave,termios.TIOCSWINSZ,struct.pack('HHHH',rows,cols,0,0))
        os.kill(pid,signal.SIGWINCH); time.sleep(.2)
    command('print 54321')
    send('continue\r'); need('SPIN_READY'); send('\x03'); need('(gdb)')
    send('print 912000+37\r'); need('912037')
    print('PASS: resize, Ctrl-C and subsequent command',flush=True)
    command('kill')
    send('quit\r')
    deadline=time.monotonic()+5
    while time.monotonic()<deadline:
        result,status=os.waitpid(pid,os.WNOHANG)
        if result:
            assert os.waitstatus_to_exitcode(status)==0,status
            pid=0; break
        if select.select([master],[],[],.1)[0]: log.write(os.read(master,65536))
    assert pid==0, 'exit hung'
    assert termios.tcgetattr(slave)==before, 'terminal attributes changed'
    print('PASS: quit, exact terminal attribute restoration',flush=True)
finally:
    if pid:
        try: os.killpg(pid,signal.SIGKILL)
        except ProcessLookupError: pass
        os.waitpid(pid,0)
    log.close(); os.close(master); os.close(slave)
    workspace.cleanup()
