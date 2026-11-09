/* dasm <dir> <hexstart> <hexend>: disassembles the original code (Musashi's disassembler) with the harness's memory image. */
#include <stdio.h>
#include <stdlib.h>
#include "harness.h"
#include "m68k.h"
int main(int argc, char **argv)
{
    if (argc < 4 || image_load(argv[1])) return 2;
    unsigned a = (unsigned)strtoul(argv[2], 0, 16), e = (unsigned)strtoul(argv[3], 0, 16);
    char buf[128];
    while (a < e) {
        unsigned n = m68k_disassemble(buf, a, M68K_CPU_TYPE_68020);
        printf("%06X  %s\n", a, buf);
        a += n;
    }
    return 0;
}
