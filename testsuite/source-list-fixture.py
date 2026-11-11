"""Generate a large source-list fixture for an x86-64 GNU assembler.

python3 source-list-fixture.py /tmp/source-list 50000
gcc -g /tmp/source-list/main.c /tmp/source-list/files.s -o /tmp/source-list/program
expect source-dialog.exp /path/to/cgdb /path/to/gdb /tmp/source-list/program

The extra source paths intentionally do not exist. Include an ordinary C main
so GDB can find the initial source without scanning every synthetic file.
"""
from pathlib import Path
import sys

output = Path(sys.argv[1])
count = int(sys.argv[2]) if len(sys.argv) > 2 else 10000
output.mkdir(parents=True, exist_ok=True)
(output / "main.c").write_text(
    "extern int fixture_files(void); int main(void) { return fixture_files(); }\n"
)
with (output / "files.s").open("w") as assembly:
    assembly.write(".text\n.globl fixture_files\nfixture_files:\n")
    for i in range(1, count + 1):
        assembly.write(
            f'.file {i} "/cgdb-source-list/dir{i % 100}/file{i}.cpp"\n'
            f".loc {i} 1 0\nnop\n"
        )
    assembly.write("xor %eax,%eax\nret\n")
