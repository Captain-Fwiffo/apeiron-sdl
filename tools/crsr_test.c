/* crsr_test <data_dir>: decodes the colour cursor 128 of the Apeiron file and prints it as text (. transparent, # dark, o light) with its hot spot */
#include <stdio.h>
#include <stdlib.h>
#include "../src/cicn.h"
#include "../src/rsrc.h"

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    ResFile rf; char err[256], msg[256];
    if (rsrc_open(&rf, argv[1], "Apeiron", err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    const Resource *r = rsrc_find(&rf, "crsr", 128);
    CicnImage im; int hx, hy;
    if (!r || crsr_decode(r->data, r->size, &im, &hx, &hy, msg, sizeof msg)) { fprintf(stderr, "crsr: %s\n", r ? msg : "missing"); return 1; }
    printf("%dx%d hot spot (%d, %d)\n", im.width, im.height, hx, hy);
    for (int y = 0; y < im.height; y++) {
        for (int x = 0; x < im.width; x++) {
            uint32_t p = im.argb[y * im.width + x];
            unsigned l = ((p >> 16 & 255) + (p >> 8 & 255) + (p & 255)) / 3;
            putchar(!(p >> 24) ? '.' : l < 100 ? '#' : l < 200 ? 'o' : 'O');
        }
        putchar('\n');
    }
    return 0;
}
