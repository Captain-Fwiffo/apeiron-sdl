/* apeiron_icon <data_dir> <out.ico>: writes the application icon of the user's Apeiron file as a Windows .ico (32 x 32, 32-bit with alpha).
 * build_windows.ps1 uses it to give apeiron.exe the icon; the file stays on the user's disk (it is the original's artwork). */
#include <stdio.h>
#include <stdint.h>
#include "../src/cicn.h"
#include "../src/rsrc.h"

static void w16(FILE *f, unsigned v) { fputc(v & 255, f); fputc(v >> 8 & 255, f); }
static void w32(FILE *f, unsigned v) { w16(f, v & 0xFFFF); w16(f, v >> 16); }

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: apeiron_icon <data_dir> <out.ico>\n"); return 2; }
    ResFile rf; char err[256]; CicnImage im;
    if (rsrc_open(&rf, argv[1], "Apeiron", err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    const Resource *i8 = rsrc_find(&rf, "icl8", 128), *m = rsrc_find(&rf, "ICN#", 128);
    if (!i8 || !m || icon_decode(i8->data, i8->size, m->data, m->size, &im)) { fprintf(stderr, "no application icon in the Apeiron file\n"); return 1; }
    FILE *f = fopen(argv[2], "wb");
    if (!f) return 1;
    w16(f, 0); w16(f, 1); w16(f, 1);                                   /* ICONDIR: one image */
    fputc(32, f); fputc(32, f); fputc(0, f); fputc(0, f); w16(f, 1); w16(f, 32); w32(f, 40 + 32 * 32 * 4 + 32 * 4); w32(f, 22);
    w32(f, 40); w32(f, 32); w32(f, 64); w16(f, 1); w16(f, 32); w32(f, 0); w32(f, 32 * 32 * 4 + 32 * 4); w32(f, 0); w32(f, 0); w32(f, 0); w32(f, 0);   /* BITMAPINFOHEADER, height doubled */
    for (int y = 31; y >= 0; y--) for (int x = 0; x < 32; x++) w32(f, im.argb[y * 32 + x]);                                                           /* bottom-up pixels */
    for (int i = 0; i < 32; i++) w32(f, 0);                                                                                                         /* AND mask unused (alpha is used) */
    fclose(f);
    cicn_free(&im); rsrc_close(&rf);
    return 0;
}
