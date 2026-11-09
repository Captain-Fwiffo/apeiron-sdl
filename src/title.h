/* The title screen (seg4_08E6 draws it; seg4_1114 / seg4_1522 / seg4_0378 handle the mouse and the keys): the logo, the five menu buttons, the sound
 * volume and music rows with their arrow buttons, the high-score table and the static picture, laid out as the original does it in its 640 x 480 window.
 * The text is drawn with the port's own small font (the original used the Mac fonts New York 18 and Geneva 9), so letter shapes differ. */
#ifndef APEIRON_TITLE_H
#define APEIRON_TITLE_H

#include <stdint.h>

#include "cicn.h"
#include "pict.h"
#include "scores.h"

#define TITLE_W 640
#define TITLE_H 480

typedef struct {
    PictImage logo, noise;        /* Apeiron Titles PICT 400 and 402 */
    CicnImage icon[4];            /* cicn 1000 right arrow, 1001 its pressed look, 1002 left arrow, 1003 its pressed look */
    char str[40][96];             /* STR# 130, index 1..39 (Mac Roman turned into plain text) */
    int ok;
} TitleRes;

/* Button numbers: the menu (Play, Instructions, Clear Scores, About, Quit), then volume down and up, music left and right. */
enum { TB_PLAY, TB_INSTR, TB_CLEAR, TB_ABOUT, TB_QUIT, TB_VOL_DOWN, TB_VOL_UP, TB_MUSIC_LEFT, TB_MUSIC_RIGHT, TB_COUNT };

int title_load(TitleRes *t, const char *dir);        /* 0 on success */
void title_free(TitleRes *t);
/* Draws the whole screen into fb (TITLE_W x TITLE_H, 0xAARRGGBB). highlight: the table row to show in white (-1 none); pressed: the button shown pressed (-1 none). */
void title_draw(uint32_t *fb, const TitleRes *t, const ScoreTable *s, int highlight, int pressed);
/* The button under a point, or -1. */
int title_hit(int x, int y);
/* The rectangle of a button (left, top, right, bottom). */
void title_button_rect(int b, int *l, int *t, int *r, int *bt);

#endif
