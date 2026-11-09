#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <io.h>
#include <windows.h>

__attribute__((noinline)) void checkpoint(int n)
{
    asm volatile("" : : "r"(n) : "memory");
}

int main(int argc, char **argv)
{
    FILE *fp = stdout;
    DWORD mode = 0;
    std::fprintf(stderr, "CONSOLE_%d\n",
        !!GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode));
    if (argc > 1 && std::strcmp(argv[1], "spin") == 0) {
        std::fprintf(fp, "INTERRUPT_READY\n");
        for (;;) Sleep(100);
    }
    std::fprintf(fp, "FPRINTF_%d\n", 101);
    checkpoint(1);
    std::cout << "CPP_" << 202 << '\n';
    checkpoint(2);
    if (argc > 1 && std::strcmp(argv[1], "output") == 0) return 0;
    std::fprintf(fp, "INPUT_READY\n");
    int value = 0;
    char line[64];
    if (!std::fgets(line, sizeof(line), stdin)) return 2;
    value = std::atoi(line);
    std::fprintf(fp, "READ_%d\n", value);
    checkpoint(3);
    return 0;
}
