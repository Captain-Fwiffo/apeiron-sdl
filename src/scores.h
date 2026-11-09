/* The "Apeiron Scores" file: one resource, 'ApPr' id 128, 2882 bytes, kept in a classic Mac resource fork (type 'ApFl', creator 'Apei').
 *
 * Layout (all numbers big-endian, found from the original code and checked on a real file):
 *   0x000  Str255   last name typed (the default for the next high score)          256 bytes
 *   0x100  int16    sound volume 0..3 (Off, Soft, Moderate, Loud)
 *   0x102  int16    third setting, 0 or more, default 1 (not used by anything seen yet)
 *   0x104  int16    music, 0 = No Music, 1 = Music On
 *   0x106  10 x Str255  player names, best first (Pascal string: length byte, then characters; the rest of the 256 bytes is leftover memory)
 *   0xB06  10 x int32   scores
 *   0xB2E  10 x int16   waves
 * The leftover bytes after each name are kept untouched when a table is written back, so a saved file differs from a loaded one only where it should.
 */
#ifndef APEIRON_SCORES_H
#define APEIRON_SCORES_H

#include <stddef.h>
#include <stdint.h>

#define SCORES_SIZE 2882
#define SCORES_ENTRIES 10

typedef struct {
    uint8_t raw[SCORES_SIZE];
    int16_t volume, setting2, music;
    char last_name[256];
    char name[SCORES_ENTRIES][256];
    int32_t score[SCORES_ENTRIES];
    int16_t wave[SCORES_ENTRIES];
} ScoreTable;

/* Parses the resource data. Returns 0 on success, -1 if the size is wrong. */
int scores_parse(ScoreTable *t, const uint8_t *data, size_t len);
/* The position a score takes (the first stored score it beats), or -1 if it does not make the table (seg3_16B4). */
int scores_position(const ScoreTable *t, int32_t score);
/* Puts a new entry at pos and moves the ones below it down one place, the last one drops off; the typed name also becomes the default for next time (seg8_016A). */
void scores_insert(ScoreTable *t, int pos, const char *name, int32_t score, int16_t wave);
/* Every entry back to the given name, score 0, wave 1 (the Clear Scores button); the last name is left as it is. */
void scores_clear(ScoreTable *t, const char *name);
/* Writes the decoded fields back into t->raw (names are changed only in their Pascal string part). */
void scores_pack(ScoreTable *t);

#endif
