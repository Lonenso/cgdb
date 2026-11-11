#!/usr/bin/env python3
"""PTY regression with a slow MI peer: python3 source-dialog.py cgdb/cgdb."""
import os
from pathlib import Path
import pty
import select
import signal
import struct
import sys
import tempfile
import termios
import fcntl
import time


def fake_gdb():
    channel = next(arg.split(' ', 2)[2] for arg in sys.argv
                   if arg.startswith('new-ui mi '))
    fd = os.open(channel, os.O_RDWR)
    os.write(fd, b'(gdb)\n')
    print('(gdb) ', flush=True)
    buffers = {0: b'', fd: b''}
    due = None
    while True:
        for ready in select.select(list(buffers), [], [], .02)[0]:
            data = os.read(ready, 65536)
            if not data:
                return
            buffers[ready] += data
            while b'\n' in buffers[ready]:
                line, buffers[ready] = buffers[ready].split(b'\n', 1)
                if ready == 0:
                    if line.strip() == b'quit':
                        return
                    print('CONSOLE_READY\n(gdb) ', flush=True)
                elif line.startswith(b'-file-list-exec-source-files'):
                    with open(os.environ['QUERY_LOG'], 'a') as log:
                        log.write('query\n')
                    due = time.monotonic() + 2
                elif line.startswith(b'-break-info'):
                    os.write(fd, b'^done,BreakpointTable={nr_rows="0",nr_cols="0",body=[]}\n(gdb)\n')
                else:
                    os.write(fd, b'^error,msg="No frame"\n(gdb)\n')
        if due and time.monotonic() >= due:
            mode = os.environ['SOURCE_MODE']
            if mode == 'error':
                response = '^error,msg="source query failed"\n'
            else:
                count = 100000 if mode == 'large' else 0
                response = '^done,files=[' + ','.join(
                    '{file="unit%06d.c",fullname="/src/unit%06d.c"}' % (i, i)
                    for i in range(count)) + ']\n'
            data = (response + '(gdb)\n').encode()
            while data:
                data = data[os.write(fd, data):]
            Path(os.environ['QUERY_LOG'] + '.done').touch()
            due = None


def check(binary, mode):
    with tempfile.TemporaryDirectory(prefix='cgdb-sources-') as folder:
        root = Path(folder)
        peer = root / 'gdb'
        peer.write_text('#!/bin/sh\nexec ' + sys.executable + ' ' +
                        str(Path(__file__).resolve()) + ' "$@"\n')
        peer.chmod(0o755)
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, 120, 0, 0))
        pid = os.fork()
        if pid == 0:
            os.setsid()
            fcntl.ioctl(slave, termios.TIOCSCTTY, 0)
            for fd in (0, 1, 2):
                os.dup2(slave, fd)
            os.close(master)
            os.close(slave)
            env = dict(os.environ, HOME=folder, TERM='xterm', LC_ALL='C',
                       QUERY_LOG=str(root / 'queries'), SOURCE_MODE=mode)
            env.pop('CGDB_DIR', None)
            os.execve(binary, [binary, '-d', str(peer), '-q'], env)
        os.close(slave)
        output = b''

        def pump(seconds):
            nonlocal output
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if select.select([master], [], [], .02)[0]:
                    output += os.read(master, 65536)

        def need(text, timeout=5):
            deadline = time.monotonic() + timeout
            while text not in output:
                assert time.monotonic() < deadline, (mode, text, output[-1500:])
                pump(.02)

        def send(text):
            nonlocal output
            pump(.05)
            output = b''
            os.write(master, text)

        try:
            need(b'(gdb)')
            send(b'\x1b')
            pump(.15)
            start = time.monotonic()
            send(b'o')
            need(b'Loading source files', .8)
            latency = time.monotonic() - start
            send(b'q')
            send(b'o')
            need(b'Loading source files', .8)
            send(b'q')
            send(b'i')
            send(b'echo test\r')
            need(b'CONSOLE_READY', .8)
            # Let the delayed response finish while focus stays in GDB.
            deadline = time.monotonic() + 15
            while not (root / 'queries.done').exists():
                assert time.monotonic() < deadline
                pump(.05)
            pump(.3)
            assert b'Select a file' not in output
            assert (root / 'queries').read_text() == 'query\n'
            send(b'echo still-console\r')
            need(b'CONSOLE_READY', .8)
            send(b'\x1b')
            pump(.15)
            send(b'o')
            if mode == 'large':
                need(b'Select a file', .8)
                send(b'100000G')
                need(b'100000', .8)  # Curses may repaint only the changed filename digits.
                assert (root / 'queries').read_text() == 'query\n'
            else:
                need(b'No source files reported', 5)
                send(b'/jn\r')  # Empty lists must tolerate search/navigation.
                pump(.1)
            send(b'q')
            send(b'i')
            send(b'quit\r')
            deadline = time.monotonic() + 5
            while True:
                done, status = os.waitpid(pid, os.WNOHANG)
                if done:
                    pid = 0
                    assert os.waitstatus_to_exitcode(status) == 0
                    break
                assert time.monotonic() < deadline
                time.sleep(.02)
            print('PASS:', mode, 'open %.3fs; cancel, focus, dedup, reopen' % latency)
        finally:
            if pid:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
            os.close(master)


if any(arg.startswith('new-ui mi ') for arg in sys.argv):
    fake_gdb()
else:
    for case in ('large', 'empty', 'error'):
        check(str(Path(sys.argv[1]).resolve()), case)
