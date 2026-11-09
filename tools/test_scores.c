/* test_scores <data_dir> <score> <wave> <name>: inserts a score into the table of the scores file in data_dir and writes it back (use a COPY of the file) */
#include <stdio.h>
#include <stdlib.h>
#include "../src/rsrc.h"
#include "../src/scores.h"

int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "usage: %s <data_dir> <score> <wave> <name>\n", argv[0]); return 2; }
    ResFile rf;
    char err[256];
    if (rsrc_open(&rf, argv[1], "Apeiron Scores", err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    const Resource *r = rsrc_find(&rf, "ApPr", 128);
    static ScoreTable t;
    if (!r || scores_parse(&t, r->data, r->size)) { fprintf(stderr, "no scores\n"); return 1; }
    int32_t score = atoi(argv[2]);
    int pos = scores_position(&t, score);
    printf("position %d\n", pos);
    if (pos >= 0) {
        scores_insert(&t, pos, argv[4], score, (int16_t)atoi(argv[3]));
        printf("patch: %d\n", rsrc_patch(&rf, r, t.raw));
    }
    rsrc_close(&rf);
    return 0;
}
