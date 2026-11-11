#!/usr/bin/env python3
"""POSIX PTY regression: python3 source-dialog.py cgdb/cgdb [case ...].

The MI peer waits for an explicit release, independently of machine speed.
Reported latency includes test polling; this is not a real-GDB benchmark.
"""
import os
import shlex
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
    pending = False
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
                    pending = True
                elif line.startswith(b'-break-info'):
                    os.write(fd, b'^done,BreakpointTable={nr_rows="0",nr_cols="0",body=[]}\n(gdb)\n')
                else:
                    os.write(fd, b'^error,msg="No frame"\n(gdb)\n')
        if pending and Path(os.environ['QUERY_LOG'] + '.release').exists():
            mode = os.environ['SOURCE_MODE']
            if mode == 'error':
                response = '^error,msg="source query failed"\n'
            else:
                count = 100000 if mode in ('large', 'narrow', 'search') else 0
                response = '^done,files=[' + ','.join(
                    '{file="unit%06d.c",fullname="/src/unit%06d.c"}' % (i, i)
                    for i in range(count)) + ']\n'
            data = (response + '(gdb)\n').encode()
            while data:
                data = data[os.write(fd, data):]
            Path(os.environ['QUERY_LOG'] + '.done').touch()
            pending = False


def check(binary, mode):
    with tempfile.TemporaryDirectory(prefix='cgdb-sources-') as folder:
        root = Path(folder)
        peer = root / 'gdb'
        peer.write_text('#!/bin/sh\nexec ' + shlex.quote(sys.executable) + ' ' +
                        shlex.quote(str(Path(__file__).resolve())) + ' "$@"\n')
        peer.chmod(0o755)
        master, slave = pty.openpty()
        width = 24 if mode == 'narrow' else 120
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 40, width, 0, 0))
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
                    data = os.read(master, 65536)
                    assert data, (mode, 'unexpected EOF')
                    output += data

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
            need(b'Select a file (loading', 2)
            latency = time.monotonic() - start
            if mode == 'search':
                # Withhold the reply until search editing is active. It must
                # arrive without Enter; the typed regex cannot satisfy need().
                send(b'/unit099999[.]c')
                (root / 'queries.release').touch()
                need(b'unit099999.c', 15)
                send(b'\x1b')
                pump(.1)
            send(b'q')
            send(b'o')
            need(b'Select a file', 2)
            send(b'q')
            send(b'i')
            send(b'echo test\r')
            need(b'CONSOLE_READY', .8)
            # Release only after cancellation and a console round trip. This
            # avoids relying on timing to prove that late replies keep focus.
            (root / 'queries.release').touch()
            # Let the delayed response finish while focus stays in GDB.
            deadline = time.monotonic() + 15
            while not (root / 'queries.done').exists():
                assert time.monotonic() < deadline
                pump(.05)
            pump(.3)
            assert b'Select a file' not in output, 'late reply reopened dialog'
            assert (root / 'queries').read_text() == 'query\n', 'duplicate source query'
            send(b'echo still-console\r')
            need(b'CONSOLE_READY', .8)
            send(b'\x1b')
            pump(.15)
            send(b'o')
            if mode in ('large', 'narrow', 'search'):
                need(b'Select a file', .8)
                send(b'100000G')
                need(b'100000', 2)  # Curses may repaint only the changed filename digits.
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
    # Optional case names also allow focused ablation runs.
    for case in sys.argv[2:] or ('large', 'empty', 'error', 'narrow', 'search'):
        check(str(Path(sys.argv[1]).resolve()), case)
