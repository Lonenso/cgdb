#!/usr/bin/env python3
"""Compare the actual terminal before/after shell job-control suspend/fg.

Requires Bash, Expect and Python pyte. PROGRAM must have debug info for main.
Run with CGDB GDB PROGRAM; --expect selects a particular Expect executable.
"""
import argparse
import os
from pathlib import Path
import re
import shlex
import signal
import subprocess
import tempfile

import pyte


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cgdb", type=Path)
    parser.add_argument("gdb", type=Path)
    parser.add_argument("program", type=Path)
    parser.add_argument("--expect", default="expect")
    args = parser.parse_args()
    command = [args.cgdb.resolve().as_posix(), "-d",
               args.gdb.resolve().as_posix(), "-nx", "-q"]
    if os.environ.get("GDB_DATA_DIRECTORY"):
        command.append("--data-directory=" + os.environ["GDB_DATA_DIRECTORY"])
    command.append(args.program.resolve().as_posix())
    with tempfile.TemporaryDirectory(prefix="cgdb-suspend-") as folder:
        env = dict(os.environ, TERM="screen-256color", HOME=folder,
                   CGDB_DIR=folder, CGDB_TEST_COMMAND=shlex.join(command),
                   DEBUGINFOD_URLS="")
        # File output avoids inherited pipe handles preventing timeout cleanup.
        with tempfile.TemporaryFile() as log:
            proc = subprocess.Popen(
                [args.expect, str(Path(__file__).with_suffix(".exp"))],
                env=env, stdout=log, stderr=subprocess.STDOUT,
                start_new_session=(os.name == "posix"))
            try:
                result = proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                if os.name == "posix":
                    os.killpg(proc.pid, signal.SIGKILL)
                else:
                    subprocess.run(["taskkill.exe", "/PID", str(proc.pid),
                                    "/T", "/F"], check=False,
                                   stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL)
                proc.wait()
                raise
            log.seek(0)
            output = log.read()
        if result:
            raise AssertionError(output.decode("utf-8", errors="replace"))
        screen = pyte.Screen(120, 40)
        stream = pyte.Stream(screen)
        snapshots = {}
        # Markers are test output, not bytes delivered to the terminal.
        chunks = re.split(rb"(SNAPSHOT_(?:BEFORE|AFTER)_\d)\r?\n", output)
        for index, chunk in enumerate(chunks):
            if index % 2:
                snapshots[chunk.decode()] = list(screen.display)
            else:
                stream.feed(chunk.decode("utf-8", errors="replace"))
        for cycle in range(3):
            before = snapshots[f"SNAPSHOT_BEFORE_{cycle}"]
            after = snapshots[f"SNAPSHOT_AFTER_{cycle}"]
            changed = [i + 1 for i, (a, b) in enumerate(zip(before, after))
                       if a != b]
            assert not changed, f"fg cycle {cycle + 1}: changed rows {changed}"
        print("PASS: three suspend/fg screen comparisons and live GDB commands")


if __name__ == "__main__":
    main()
