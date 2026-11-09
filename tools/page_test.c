/* page_test <data_dir> <pict id> <out.ppm>: draws one of the instruction pages */
#include <stdio.h>
#include <stdlib.h>
#include "../src/pictdraw.h"
#include "../src/rsrc.h"

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    ResFile rf; char err[256], msg[256];
    if (rsrc_open(&rf, argv[1], "Apeiron Titles", err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    const Resource *r = rsrc_find(&rf, "PICT", atoi(argv[2]));
    if (!r) return 1;
    static uint32_t fb[640 * 480];
    for (int i = 0; i < 640 * 480; i++) fb[i] = 0xFF000000u;
    int rc = pict_draw(r->data, r->size, fb, 640, 480, NULL, msg, sizeof msg);
    printf("rc %d %s\n", rc, rc ? msg : "");
    FILE *f = fopen(argv[3], "wb"); fprintf(f, "P6\n640 480\n255\n");
    for (int i = 0; i < 640 * 480; i++) { fputc(fb[i] >> 16 & 255, f); fputc(fb[i] >> 8 & 255, f); fputc(fb[i] & 255, f); }
    fclose(f);
    return 0;
}
