#include "title.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "rsrc.h"

#define GRAY   0xFF9A9393u      /* the grey of the lines and the table rows (the nearest entry of the game's table to the original's blue-grey) */
#define WHITE  0xFFFFFFFFu
#define YELLOW 0xFFF9F931u

int title_load(TitleRes *t, const char *dir)
{
    ResFile rf;
    char err[256], msg[256];
    memset(t, 0, sizeof *t);
    if (rsrc_open(&rf, dir, "Apeiron Titles", err, sizeof err)) return 1;
    const Resource *r = rsrc_find(&rf, "PICT", 400);
    if (!r || pict_decode(r->data, r->size, &t->logo, msg, sizeof msg) != PICT_OK || !t->logo.indexed) { rsrc_close(&rf); return 1; }
    r = rsrc_find(&rf, "PICT", 402);
    if (r) pict_decode(r->data, r->size, &t->noise, msg, sizeof msg);
    rsrc_close(&rf);
    if (rsrc_open(&rf, dir, "Apeiron", err, sizeof err)) return 1;
    for (int k = 0; k < 4; k++) {
        r = rsrc_find(&rf, "cicn", 1000 + k);
        if (!r || cicn_decode(r->data, r->size, &t->icon[k], msg, sizeof msg)) { rsrc_close(&rf); return 1; }
    }
    r = rsrc_find(&rf, "STR#", 130);
    if (r && r->size >= 2) {
        unsigned n = (unsigned)(r->data[0] << 8 | r->data[1]), p = 2;
        for (unsigned k = 1; k <= n && k < 40 && p < r->size; k++) {
            unsigned len = r->data[p];
            if (p + 1 + len > r->size) break;
            char *o = t->str[k];
            size_t m = 0;
            for (unsigned i = 0; i < len && m < 90; i++) {
                unsigned char c = r->data[p + 1 + i];
                if (c == 0xA9) o[m++] = (char)0xA9;                                  /* the copyright sign, which the font tables have */
                else if (c < 32 || c > 126) o[m++] = '?';
                else o[m++] = (char)c;
            }
            o[m] = 0;
            p += 1 + len;
        }
    }
    rsrc_close(&rf);
    t->ok = 1;
    return 0;
}

void title_free(TitleRes *t)
{
    pict_free(&t->logo);
    pict_free(&t->noise);
    for (int k = 0; k < 4; k++) cicn_free(&t->icon[k]);
    memset(t, 0, sizeof *t);
}

static void put_pict(uint32_t *fb, const PictImage *img, int x0, int y0)
{
    if (!img->pixels || !img->indexed) return;
    for (int y = 0; y < img->height; y++)
        for (int x = 0; x < img->width; x++) {
            int xx = x0 + x, yy = y0 + y;
            if (xx < 0 || yy < 0 || xx >= TITLE_W || yy >= TITLE_H) continue;
            const uint8_t *c = &img->palette[img->pixels[y * img->width + x] * 3];
            fb[yy * TITLE_W + xx] = 0xFF000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
        }
}

static void put_icon(uint32_t *fb, const CicnImage *im, int x0, int y0)
{
    for (int y = 0; y < im->height; y++)
        for (int x = 0; x < im->width; x++) {
            int xx = x0 + x, yy = y0 + y;
            if (xx < 0 || yy < 0 || xx >= TITLE_W || yy >= TITLE_H) continue;
            uint32_t p = im->argb[y * im->width + x];
            if (p >> 24) fb[yy * TITLE_W + xx] = p;
        }
}

static void hline(uint32_t *fb, int x0, int x1, int y, uint32_t c) { for (int x = x0; x < x1; x++) if (x >= 0 && x < TITLE_W && y >= 0 && y < TITLE_H) fb[y * TITLE_W + x] = c; }
static void vline(uint32_t *fb, int x, int y0, int y1, uint32_t c) { for (int y = y0; y < y1; y++) if (x >= 0 && x < TITLE_W && y >= 0 && y < TITLE_H) fb[y * TITLE_W + x] = c; }

/* text with the pen at (x, baseline): the letters sit on the baseline; scale 2 is the big serif, scale 1 the small sans (bold or not) */
static int text(uint32_t *fb, int x, int baseline, int scale, uint32_t color, int bold, const char *s)
{
    const FFont *f = scale >= 2 ? &font_serif18 : bold ? &font_sans9b : &font_sans9;      /* New York 18 / Geneva 9 (bold) stand-ins from open fonts */
    return ffont_draw(fb, TITLE_W, TITLE_H, x, baseline, f, color, s);
}

void title_button_rect(int b, int *l, int *t, int *r, int *bt)
{
    int x, y;
    if (b < TB_VOL_DOWN) { x = 380 + 30; y = 30 + 40 * b; }                                  /* the menu: a column at x = 410, 40 apart */
    else { int row = (b - TB_VOL_DOWN) / 2, right = (b - TB_VOL_DOWN) & 1; x = 410 + (right ? 120 : 0); y = 265 + 45 * row; }   /* the two rows: the arrows 120 apart */
    *l = x; *t = y; *r = x + 32; *bt = y + 32;
}

int title_hit(int x, int y)
{
    for (int b = 0; b < TB_COUNT; b++) {
        int l, t, r, bt;
        title_button_rect(b, &l, &t, &r, &bt);
        if (x >= l && x < r && y >= t && y < bt) return b;
    }
    return -1;
}

void title_draw(uint32_t *fb, const TitleRes *t, const ScoreTable *s, int highlight, int pressed)
{
    for (int i = 0; i < TITLE_W * TITLE_H; i++) fb[i] = 0xFF000000u;
    put_pict(fb, &t->logo, 0, 0);
    hline(fb, 0, 380, 150, GRAY); hline(fb, 0, 380, 405, GRAY);
    vline(fb, 380, 0, TITLE_H, GRAY);
    hline(fb, 380, TITLE_W, 250, GRAY); hline(fb, 380, TITLE_W, 345, GRAY);
    text(fb, 2, 148, 1, WHITE, 0, "1.0.2");
    text(fb, 2, 478, 1, WHITE, 1, t->str[1]);                                        /* the copyright line */
    /* the menu */
    for (int b = 0; b < TB_QUIT + 1; b++) {
        int l, tp, r, bt;
        title_button_rect(b, &l, &tp, &r, &bt);
        put_icon(fb, &t->icon[pressed == b ? 1 : 0], l, tp);
        text(fb, r + 7, bt - 9, 2, WHITE, 0, t->str[8 + b]);
    }
    /* sound volume and music */
    for (int row = 0; row < 2; row++) {
        int bl = TB_VOL_DOWN + 2 * row, l, tp, r, bt;
        title_button_rect(bl, &l, &tp, &r, &bt);
        put_icon(fb, &t->icon[pressed == bl ? 3 : 2], l, tp);
        title_button_rect(bl + 1, &l, &tp, &r, &bt);
        put_icon(fb, &t->icon[pressed == bl + 1 ? 1 : 0], l, tp);
        title_button_rect(bl, &l, &tp, &r, &bt);
        text(fb, l - 10, tp - 2, 1, GRAY, 1, t->str[14 + row]);
        const char *v = t->str[row == 0 ? 22 + (s->volume & 3) : 26 + (s->music ? 1 : 0)];
        text(fb, l + 16 + (120 - ffont_width(&font_sans9b, v)) / 2, tp + 20, 1, WHITE, 1, v);
    }
    /* the high scores */
    static const int hx[3] = { 12, 232, 315 }, hs[3] = { 6, 7, 13 };
    for (int k = 0; k < 3; k++) { int w = text(fb, hx[k], 185, 2, YELLOW, 0, t->str[hs[k]]); hline(fb, hx[k], hx[k] + w, 187, YELLOW); }
    for (int i = 0; i < SCORES_ENTRIES; i++) {
        uint32_t c = i == highlight ? WHITE : GRAY;
        int y = 185 + 20 * (i + 1);
        char name[64], num[16];
        snprintf(name, sizeof name, "%s", s->name[i]);
        if (20 + ffont_width(&font_serif18, name) > 212)                                                       /* too wide: shortened, with dots */
            for (int n = (int)strlen(s->name[i]); n > 0; n--) { snprintf(name, sizeof name, "%.*s...", n, s->name[i]); if (20 + ffont_width(&font_serif18, name) <= 212) break; }
        text(fb, 20, y, 2, c, 0, name);
        snprintf(num, sizeof num, "%d", (int)s->score[i]);
        text(fb, 282 - ffont_width(&font_serif18, num), y, 2, c, 0, num);
        snprintf(num, sizeof num, "%d", (int)s->wave[i]);
        text(fb, 365 - ffont_width(&font_serif18, num), y, 2, c, 0, num);
    }
    put_pict(fb, &t->noise, 381, 346);
}
