/* title_test <data_dir> <out.ppm>: draws the title screen once */
#include <stdio.h>
#include <stdlib.h>
#include "../src/rsrc.h"
#include "../src/title.h"

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    static TitleRes t;
    static ScoreTable s;
    static uint32_t fb[TITLE_W * TITLE_H];
    ResFile rf; char err[256];
    if (title_load(&t, argv[1])) { fprintf(stderr, "title resources missing\n"); return 1; }
    if (rsrc_open(&rf, argv[1], "Apeiron Scores", err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    const Resource *r = rsrc_find(&rf, "ApPr", 128);
    if (!r || scores_parse(&s, r->data, r->size)) return 1;
    title_draw(fb, &t, &s, argc > 3 ? atoi(argv[3]) : 2, -1);
    FILE *f = fopen(argv[2], "wb"); fprintf(f, "P6\n640 480\n255\n");
    for (int i = 0; i < TITLE_W * TITLE_H; i++) { fputc(fb[i] >> 16 & 255, f); fputc(fb[i] >> 8 & 255, f); fputc(fb[i] & 255, f); }
    fclose(f);
    return 0;
}
