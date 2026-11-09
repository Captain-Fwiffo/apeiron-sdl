/* cicn_test <data_dir> <out.ppm>: decodes the colour icons of the Apeiron file and writes them side by side on a grey background */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/cicn.h"
#include "../src/rsrc.h"

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    ResFile rf; char err[256], msg[256];
    if (rsrc_open(&rf, argv[1], "Apeiron", err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    static uint32_t sheet[600 * 120];
    for (int i = 0; i < 600 * 120; i++) sheet[i] = 0xFF808080u;
    int x0 = 4;
    for (int k = 0; k < rf.count; k++) {
        if (memcmp(rf.res[k].type, "cicn", 4)) continue;
        CicnImage im;
        if (cicn_decode(rf.res[k].data, rf.res[k].size, &im, msg, sizeof msg)) { printf("cicn %d: %s\n", rf.res[k].id, msg); continue; }
        printf("cicn %d: %dx%d\n", rf.res[k].id, im.width, im.height);
        for (int y = 0; y < im.height && y < 120; y++) for (int x = 0; x < im.width; x++) if ((im.argb[y * im.width + x] >> 24) && x0 + x < 600) sheet[y * 600 + x0 + x] = im.argb[y * im.width + x];
        x0 += im.width + 8;
        cicn_free(&im);
    }
    for (int k = 0; k < rf.count; k++) {         /* the application icons too: icl8 + ICN# of the same id */
        if (memcmp(rf.res[k].type, "icl8", 4)) continue;
        const Resource *m = rsrc_find(&rf, "ICN#", rf.res[k].id); CicnImage im;
        if (!m || icon_decode(rf.res[k].data, rf.res[k].size, m->data, m->size, &im)) continue;
        printf("icl8 %d: 32x32\n", rf.res[k].id);
        for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) if ((im.argb[y * 32 + x] >> 24) && x0 + x < 600) sheet[(70 + y) * 600 + x0 + x] = im.argb[y * 32 + x];
        x0 += 40; cicn_free(&im);
    }
    FILE *f = fopen(argv[2], "wb"); fprintf(f, "P6\n600 120\n255\n");
    for (int i = 0; i < 600 * 120; i++) { fputc(sheet[i] >> 16 & 255, f); fputc(sheet[i] >> 8 & 255, f); fputc(sheet[i] & 255, f); }
    fclose(f);
    return 0;
}
