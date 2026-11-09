/* apeiron_scores <dir>: reads "Apeiron Scores" (resource fork in any of the forms rsrc.h accepts) and prints the table.
 * Also checks that unpacking and repacking gives back the same 2882 bytes. */
#include <stdio.h>
#include <string.h>

#include "../src/rsrc.h"
#include "../src/scores.h"

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: %s <dir with Apeiron Scores>\n", argv[0]); return 2; }
    ResFile rf;
    char err[512];
    if (rsrc_open(&rf, argv[1], "Apeiron Scores", err, sizeof err)) { fprintf(stderr, "%s\n", err); return 1; }
    const Resource *r = rsrc_find(&rf, "ApPr", 128);
    if (!r) { fprintf(stderr, "no 'ApPr' 128 resource in %s\n", rf.source); return 1; }
    ScoreTable t;
    if (scores_parse(&t, r->data, r->size)) { fprintf(stderr, "'ApPr' 128 is %u bytes, expected %d\n", r->size, SCORES_SIZE); return 1; }
    printf("last name \"%s\", volume %d, setting2 %d, music %d\n", t.last_name, t.volume, t.setting2, t.music);
    for (int i = 0; i < SCORES_ENTRIES; i++) printf("%2d  %-12s %9d  wave %d\n", i + 1, t.name[i], t.score[i], t.wave[i]);
    uint8_t before[SCORES_SIZE];
    memcpy(before, t.raw, sizeof before);
    scores_pack(&t);
    printf("repack %s\n", memcmp(before, t.raw, sizeof before) == 0 ? "identical" : "DIFFERENT");
    rsrc_close(&rf);
    return 0;
}
