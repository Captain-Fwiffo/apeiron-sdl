#include "game.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "pict.h"
#include "ppat.h"
#include "rsrc.h"
#include "scores.h"
#include "snd.h"

/* ---- layout of the memory image ---- */
#define A5_WORLD   0x200000u
#define CODE_BASE  0x20000u
#define SEG_STRIDE 0x10000u
#define ENGINE     0x3A0000u
#define BOUNDS     0x3A1000u
#define ROWS_SCREEN 0x3B0000u
#define ROWS_WORK  0x3B2000u
#define ROWS_SAVED 0x3B4000u
#define GRID       0x310400u
#define GRID2      0x315400u
#define CLASS_HANDLES 0x3C0000u
#define CLASS_DATA 0x3C1000u
#define HEAP       0x500000u
#define CTAB_BASE  0xA00000u      /* the Color Tables (see build_palette_tables) */
#define CTAB_SIZE  0x1000u
#define SCREEN     0x700000u
#define WORK       0x800000u
#define SAVED      0x900000u
#define BLIT_BASE  0x7000000u

/* the 21 sprite classes: A5 offset of the global that holds the class handle, PICT in "Apeiron Sprites", frames, frame size, first frame, pitch */
static const struct { int a5; int pict, frames, w, h, x0, y0, pitch; } kClasses[GAME_CLASSES] = {
    { -0x638, 1000, 48, 16, 16, 1, 1, 17 }, { -0x634, 1001, 62, 16, 16, 1, 1, 17 }, { -0x630, 1002, 33, 16, 16, 1, 1, 17 },
    { -0x62C, 1003, 1, 4, 12, 7, 3, 17 },   { -0x628, 1004, 8, 16, 16, 1, 1, 17 },  { -0x624, 1005, 18, 42, 20, 1, 1, 43 },
    { -0x620, 1006, 20, 24, 24, 1, 1, 25 }, { -0x61C, 1007, 12, 4, 4, 1, 1, 5 },    { -0x618, 1008, 10, 8, 8, 1, 1, 9 },
    { -0x614, 1018, 10, 8, 8, 1, 1, 9 },    { -0x610, 1009, 19, 16, 16, 1, 1, 17 }, { -0x60C, 1010, 102, 8, 8, 1, 1, 9 },
    { -0x608, 1011, 16, 16, 16, 1, 1, 17 }, { -0x604, 1012, 16, 16, 16, 1, 1, 17 }, { -0x600, 1013, 28, 16, 16, 1, 1, 17 },
    { -0x5FC, 1014, 9, 32, 32, 1, 1, 33 },  { -0x5F8, 1015, 4, 14, 14, 1, 1, 15 },  { -0x5F4, 1016, 1, 4, 12, 7, 3, 17 },
    { -0x5F0, 1017, 40, 20, 20, 1, 1, 21 }, { -0x5EC, 1019, 6, 16, 16, 1, 1, 17 },  { -0x5E8, 1020, 9, 32, 32, 1, 1, 33 } };

static void remap_image(const Game *g, PictImage *img);
static void fill_backdrop(Game *g);

static uint32_t a5(const Game *g, int off) { return g->ctx.a5 + (uint32_t)off; }
static void w32(Game *g, int off, uint32_t v) { gm_wr32(&g->ctx, a5(g, off), v); }
static void w16(Game *g, int off, uint32_t v) { gm_wr16(&g->ctx, a5(g, off), (uint16_t)v); }
static void w8(Game *g, int off, uint32_t v) { gm_wr8(&g->ctx, a5(g, off), (uint8_t)v); }


/* ---- side-panel numbers and icons, and sounds ---- */
/* A5 offset of each sound global and the 'snd ' resource it stands for (from the original's loading code) */
static const struct { int a5, snd; } kSounds[] = {
    { -0x580, 129 },
    { -0x57C, 130 },
    { -0x55C, 131 },
    { -0x56C, 132 },
    { -0x584, 133 },
    { -0x564, 134 },
    { -0x568, 135 },
    { -0x560, 136 },
    { -0x570, 137 },
    { -0x574, 138 },
    { -0x578, 140 },
    { -0x558, 141 },
    { -0x554, 142 },
    { -0x550, 146 },
    { -0x54C, 147 },
    { -0x548, 148 },
    { -0x544, 149 },
    { -0x540, 151 },
    { -0x53C, 152 },
    { -0x538, 153 },
    { -0x534, 154 },
    { -0x530, 155 },
    { -0x52C, 156 },
    { -0x528, 157 },
    { -0x524, 158 },
    { -0x520, 159 },
    { -0x51C, 160 },
    { -0x518, 161 },
    { -0x514, 162 },
    { -0x510, 163 },
    { -0x50C, 164 },
    { -0x508, 165 },
    { -0x504, 166 },
    { -0x500, 167 },
    { -0x4FC, 168 },
    { -0x4F8, 169 } };

static int load_hud_art(Game *g, const char *dir)
{
    ResFile rf;
    char err[256], msg[256];
    if (rsrc_open(&rf, dir, "Apeiron Sprites", err, sizeof err)) { snprintf(g->error, sizeof g->error, "%s", err); return -1; }
    const Resource *r = rsrc_find(&rf, "PICT", 1100);
    PictImage img;
    if (r && pict_decode(r->data, r->size, &img, msg, sizeof msg) == PICT_OK && img.indexed) {
        remap_image(g, &img);
        for (int i = 0; i < 11; i++)
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 24; x++) {
                    int sx = 1 + i * 25 + x, sy = 1 + y;
                    g->digit[i][y * 24 + x] = (sx < img.width && sy < img.height) ? img.pixels[sy * img.width + sx] : 0;
                }
        pict_free(&img);
    }
    for (int k = 0; k < 7; k++) {
        r = rsrc_find(&rf, "PICT", 1101 + k);
        if (r && pict_decode(r->data, r->size, &img, msg, sizeof msg) == PICT_OK && img.indexed) {
            remap_image(g, &img);
            g->icon_w[k] = img.width; g->icon_h[k] = img.height;
            g->icon[k] = (uint8_t *)malloc((size_t)img.width * img.height);
            if (g->icon[k]) memcpy(g->icon[k], img.pixels, (size_t)img.width * img.height);
            pict_free(&img);
        }
    }
    rsrc_close(&rf);
    return 0;
}

static void load_sounds(Game *g, const char *dir)
{
    ResFile rf;
    char err[256], msg[256];
    mixer_init(&g->mixer);
    if (rsrc_open(&rf, dir, "Apeiron Sounds", err, sizeof err)) return;
    for (int i = 0; i < rf.count; i++) {
        const Resource *r = &rf.res[i];
        SndData sd;
        if (strcmp(r->type, "snd ") == 0 && snd_decode(r->data, r->size, &sd, msg, sizeof msg) == 0)
            mixer_load(&g->mixer, r->id, sd.samples, (uint32_t)sd.frames, sd.rate_hz);
    }
    rsrc_close(&rf);
    if (!rsrc_open(&rf, dir, "Apeiron Music", err, sizeof err)) {
        for (int i = 0; i < rf.count; i++) {
            const Resource *r = &rf.res[i];
            SndData sd;
            if (strcmp(r->type, "snd ") == 0 && snd_decode(r->data, r->size, &sd, msg, sizeof msg) == 0)
                mixer_load(&g->mixer, r->id, sd.samples, (uint32_t)sd.frames, sd.rate_hz);
        }
        rsrc_close(&rf);
    }
    /* the settings (seg3 FUN_000514a6): music flag at -0x708 and the volume come from the scores file; with no file, music on */
    w8(g, -0x708, 1);
    if (!rsrc_open(&rf, dir, "Apeiron Scores", err, sizeof err)) {
        const Resource *r = rsrc_find(&rf, "ApPr", 128);
        if (r && scores_parse(&g->scores, r->data, r->size) == 0) { g->scores_ok = 1; w8(g, -0x708, g->scores.music != 0); g->mixer.volume = g->scores.volume & 3; }
        rsrc_close(&rf);
    }
    for (size_t i = 0; i < sizeof kSounds / sizeof kSounds[0]; i++) w32(g, kSounds[i].a5, (uint32_t)kSounds[i].snd);
    if (getenv("APEIRON_DEBUG")) { int n = 0, missing = 0; for (size_t i = 0; i < sizeof kSounds / sizeof kSounds[0]; i++) { if (g->mixer.sound[kSounds[i].snd].samples) n++; else missing++; } fprintf(stderr, "sounds loaded: %d, missing: %d\n", n, missing); }
}

static void put_image(Game *g, uint32_t dest, const uint8_t *px, int w, int h)      /* a picture copied into the screen buffer, whole */
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t d = (dest + (uint32_t)y * GAME_SCREEN_W + (uint32_t)x) & MEM_MASK;
            if (d >= SCREEN && d < SCREEN + GAME_SCREEN_W * GAME_SCREEN_H) g->mem[d] = px[y * w + x];
        }
}

/* seg3_15E4: n in 24 x 32 digits, right to left from `dest`; with `blank` one more empty cell to the left */
static void hud_number(Game *g, uint32_t n, uint32_t dest, int blank)
{
    do {
        put_image(g, dest, g->digit[n % 10], 24, 32);
        dest -= 24;
        n /= 10;
    } while (n != 0);
    if (blank) put_image(g, dest, g->digit[10], 24, 32);
}

/* seg3_157A: eight life icons, right to left, 20 px apart */
static void hud_lives(Game *g)
{
    uint32_t dest = gm_rd32(&g->ctx, a5(g, -0x68A));
    int lives = (int16_t)gm_rd16(&g->ctx, a5(g, -0x6FA));
    for (int i = 0; i < 8; i++, dest -= 20) {
        int k = i < lives ? 0 : 1;
        if (g->icon[k]) put_image(g, dest, g->icon[k], g->icon_w[k], g->icon_h[k]);
    }
}

/* seg3_1428: the multiplier and the five power-up icons, left to right, 15 px apart */
static void hud_powerups(Game *g)
{
    GameCtx *c = &g->ctx;
    uint32_t dest = gm_rd32(c, a5(g, -0x668));
    int mult = gm_rd16(c, a5(g, -0x6AA));
    if (mult) {
        const GameClass *cl = &g->cls[16];
        int f = mult - 2;
        if (f >= 0 && f < cl->frames) {
            const uint8_t *px = &cl->pixels[(size_t)f * cl->w * cl->h];
            for (int y = 0; y < cl->h; y++)
                for (int x = 0; x < cl->w; x++) {
                    uint32_t d = (dest + (uint32_t)y * GAME_SCREEN_W + (uint32_t)x) & MEM_MASK;
                    if (px[y * cl->w + x] && d >= SCREEN && d < SCREEN + GAME_SCREEN_W * GAME_SCREEN_H) g->mem[d] = px[y * cl->w + x];
                }
        }
    } else if (g->icon[1]) {
        put_image(g, dest, g->icon[1], g->icon_w[1], g->icon_h[1]);
    }
    static const int flags[5] = { -0x66D, -0x66C, -0x66B, -0x66A, -0x669 };
    for (int i = 0; i < 5; i++) {
        dest += 15;
        int k = c->mem[a5(g, flags[i]) & MEM_MASK] ? 2 + i : 1;
        if (g->icon[k]) put_image(g, dest, g->icon[k], g->icon_w[k], g->icon_h[k]);
    }
}

/* ---- the host side of what the original asked the Mac to do ---- */

static void blit(Game *g, uint32_t code, const uint8_t *raw, int n)
{
    int k = (int)((code >> 12) & 0xFF), f = (int)((code >> 1) & 0x7FF), copy = (int)(code & 1);
    if (k >= GAME_CLASSES || f >= g->cls[k].frames) return;
    const GameClass *c = &g->cls[k];
    const uint8_t *px = &c->pixels[(size_t)f * c->w * c->h];
    uint8_t *m = g->mem;
    if (copy && n == 16) {
        int32_t src = (int32_t)be32(raw), dst = (int32_t)be32(raw + 4), srb = (int32_t)be32(raw + 8), drb = (int32_t)be32(raw + 12);
        for (int y = 0; y < c->h; y++)
            for (int x = 0; x < c->w; x++) {
                if (!px[y * c->w + x]) continue;
                uint32_t s = ((uint32_t)(src + y * srb + x)) & MEM_MASK, d = ((uint32_t)(dst + y * drb + x)) & MEM_MASK;
                if (s < SCREEN || s >= 0xA00000u || d < SCREEN || d >= 0xA00000u) continue;
                m[d] = m[s];
            }
    } else if (!copy && n == 8) {
        int32_t dst = (int32_t)be32(raw), rb = (int32_t)be32(raw + 4);
        for (int y = 0; y < c->h; y++)
            for (int x = 0; x < c->w; x++) {
                uint8_t p = px[y * c->w + x];
                uint32_t d = ((uint32_t)(dst + y * rb + x)) & MEM_MASK;
                if (!p || d < SCREEN || d >= 0xA00000u) continue;
                m[d] = p;
            }
    }
}

static void play_sound(Game *g, uint32_t id, int priority)
{
    if (g->n_sounds < 16) g->sound_log[g->n_sounds++] = id;
    if (g->audio_lock) g->audio_lock(g->audio_ud, 1);
    mixer_play(&g->mixer, (int)id, priority);
    if (g->audio_lock) g->audio_lock(g->audio_ud, 0);
}

static int in_tables(uint32_t p);
static uint32_t game_ext(void *ud, int kind, const uint8_t *raw, int n)
{
    Game *g = (Game *)ud;
    GameCtx *c = &g->ctx;
    uint32_t a = n >= 4 ? be32(raw) : 0, b = n >= 8 ? be32(raw + 4) : 0;
    if ((uint32_t)kind >= BLIT_BASE) { blit(g, (uint32_t)kind, raw, n); return 0; }
    switch ((uint32_t)kind) {
    case EXT_SOUND: play_sound(g, a, (int)(raw[4] << 8 | raw[5])); return 0;
    case EXT_EXPLOSION: spawn_explosion(c, a, b); return 0;
    case EXT_MUSHROOM: spawn_mushroom(c, (int16_t)(raw[0] << 8 | raw[1]), (int16_t)(raw[2] << 8 | raw[3])); return 0;
    case 0x4363A: spawn_sparks(c, a); return 0;
    case 0x43E26: spawn_explosion(c, a, b); return 0;
    case 0x40902: engine_activate(c, a); return 0;
    case 0x42128: add_pede_head(c); return 0;
    case 0x43A36: spawn_psychedelic(c); return 0;
    case 0x43B02: spawn_multiplier(c); return 0;
    case 0x43BD8: spawn_fodder(c); return 0;
    case 0x42256: fire_shot(c); return 0;
    case 0x42334: timed_launches(c); return 0;
    case 0x42048: shield_start(c); return 0;
    case 0x420CC: house_clean(c); return 0;
    case 0x4299A: build_round(c); return 0;
    case 0x400EA: engine_move_pass(c); return 0;
    case 0x405CE: engine_full_redraw(c); return 0;
    case 0x432F8: fill_backdrop(g); return 0;
    case 0xC0B1: memcpy(g->mem + WORK, g->mem + SAVED, GAME_FIELD_W * GAME_SCREEN_H); return 0;
    case 0xC0B2: for (int y = 0; y < GAME_SCREEN_H; y++) memcpy(g->mem + SCREEN + (size_t)y * GAME_SCREEN_W, g->mem + WORK + (size_t)y * GAME_FIELD_W, GAME_FIELD_W); return 0;
    default: break;
    }
    if (kind == (int)(c->a5 + 0x3C2u)) { play_sound(g, a, (int)(raw[4] << 8 | raw[5])); return 0; }
    if (kind == (int)(c->a5 + 0x3D2u)) {
        if (g->audio_lock) g->audio_lock(g->audio_ud, 1);
        int playing = mixer_is_playing(&g->mixer, (int)a);
        if (g->audio_lock) g->audio_lock(g->audio_ud, 0);
        if (g->busy_hook) playing = g->busy_hook(g, playing);
        return playing ? 0xFFu : 0u;
    }
    if (kind == (int)(c->a5 + 0x3DAu)) {                 /* seg7_07AE(channel): the music is channel 4 */
        if (n >= 2 && (raw[0] << 8 | raw[1]) == 4) { if (g->audio_lock) g->audio_lock(g->audio_ud, 1); mixer_music_stop(&g->mixer); if (g->audio_lock) g->audio_lock(g->audio_ud, 0); }
        return 0;
    }
    if (kind == (int)(c->a5 + 0x3E2u)) {                 /* seg7_0886(start sound, 0x50004 = priority 5 / channel 4, callback): the callback seg3_336C makes it loop */
        if (g->audio_lock) g->audio_lock(g->audio_ud, 1);
        mixer_music_start(&g->mixer, 1000, 1001);
        if (g->audio_lock) g->audio_lock(g->audio_ud, 0);
        return 0;
    }
    if (kind == 0xAA3Fu && n >= 8 && in_tables(a)) {     /* _SetEntries(0, count, table): the colour-rock flash and its restore; tables of other origin are left out */
        int count = (raw[4] << 8 | raw[5]);
        if (getenv("APEIRON_PALDEBUG")) fprintf(stderr, "SetEntries table %u count %d\n", (a - CTAB_BASE) / CTAB_SIZE, count);
        for (int i = 0; i <= count && i < 256; i++)
            for (int ch = 0; ch < 3; ch++) g->palette[i * 3 + ch] = (uint8_t)(gm_rd16(c, a + (uint32_t)i * 8 + 2 + (uint32_t)ch * 2) >> 8);
        return 0;
    }
    if (kind == (int)(c->a5 + 0x36Au)) { memcpy(g->palette, g->base_palette, sizeof g->palette); return 0; }     /* seg6_071A: back to the game's table */
    if (kind == 0x4085C) { g->paused = (int)a; return 0; }                       /* the PAUSED picture on / off */
    if (kind == 0x43348) return g->caps ? 0xFFu : 0u;
    if (kind == 0x41428) { hud_powerups(g); return 0; }
    if (kind == 0x4157A) { hud_lives(g); return 0; }
    if (kind == 0x415E4) { hud_number(g, a, b, n >= 9 && raw[8]); return 0; }
    return 0;                      /* sound-playing checks, palette, HUD and the other Mac calls: nothing to do (yet) */
}

/* The original drew every picture into an off-screen port that had the game's colour table, so the Color Manager remapped each picture's colours to the
 * nearest entry of that table. The port does the same once, here, with the palette chosen in load_palette (index 0 stays white = transparent). */
static int load_palette(Game *g, const char *dir)
{
    ResFile rf;
    char err[256];
    if (rsrc_open(&rf, dir, "Apeiron", err, sizeof err)) { snprintf(g->error, sizeof g->error, "the Apeiron application file is needed: %s", err); return -1; }
    int id = getenv("APEIRON_CLUT") ? atoi(getenv("APEIRON_CLUT")) : 128;
    const Resource *r = rsrc_find(&rf, "clut", id);
    int ok = r && clut_decode(r->data, r->size, g->palette) > 0;
    const Resource *rs = rsrc_find(&rf, "STR#", 132);
    if (rs && rs->size >= 2) {
        uint32_t p = 2;
        for (int k = 0; k < 11 && k < (rs->data[0] << 8 | rs->data[1]) && p < rs->size; k++) {
            unsigned len = rs->data[p];
            if (len > 31 || p + 1 + len > rs->size) break;
            memcpy(g->cheat_codes[k], rs->data + p, len + 1);
            p += 1 + len;
        }
    }
    const Resource *r2 = rsrc_find(&rf, "clut", 129);
    if (r2) clut_decode(r2->data, r2->size, g->title_palette);
    rsrc_close(&rf);
    if (!ok) { snprintf(g->error, sizeof g->error, "Apeiron: colour table %d missing", id); return -1; }
    g->black_index = 255;
    for (int i = 255; i >= 0; i--) if (!g->palette[i * 3] && !g->palette[i * 3 + 1] && !g->palette[i * 3 + 2]) { g->black_index = i; break; }
    return 0;
}

/* the Mac's standard 8-bit system colour table (clut 8): a 6 x 6 x 6 cube from white down to black, then ramps of red, green, blue and grey, and black last.
 * A picture saved without a colour table of its own refers to this table. */
static void system_palette(uint8_t pal[256 * 3])
{
    for (int i = 0; i < 216; i++) {
        pal[i * 3] = (uint8_t)(0xFF - 0x33 * (i % 6));            /* red varies fastest, blue slowest (as in the sprite strips' own tables) */
        pal[i * 3 + 1] = (uint8_t)(0xFF - 0x33 * ((i / 6) % 6));
        pal[i * 3 + 2] = (uint8_t)(0xFF - 0x33 * ((i / 36) % 6));
    }
    static const uint8_t ramp[10] = { 0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11 };
    for (int i = 0; i < 40; i++) {
        uint8_t v = ramp[i % 10];
        int group = i / 10;
        uint8_t *p = &pal[(216 + i) * 3];
        p[0] = group == 0 ? v : group == 3 ? v : 0;
        p[1] = group == 1 ? v : group == 3 ? v : 0;
        p[2] = group == 2 ? v : group == 3 ? v : 0;
    }
    pal[255 * 3] = pal[255 * 3 + 1] = pal[255 * 3 + 2] = 0;
}

static void remap_image(const Game *g, PictImage *img)
{
    uint8_t map[256], sys[256 * 3];
    system_palette(sys);
    if (img->palette_size < 16) return;         /* the coin and flea strips carry no colour table: their indices are the game table's own (checked: the flea comes out as designed) */
    int own = 1;
    for (int i = 0; i < 256; i++) {
        int best = 0, bd = 1 << 30;
        const uint8_t *c = (own && i < img->palette_size) ? &img->palette[i * 3] : &sys[i * 3];
        for (int j = 0; j < 256; j++) {
            int dr = c[0] - g->palette[j * 3], dg = c[1] - g->palette[j * 3 + 1], db = c[2] - g->palette[j * 3 + 2];
            int d = dr * dr + dg * dg + db * db;
            if (d < bd) { bd = d; best = j; }
        }
        map[i] = (uint8_t)best;
    }
    for (int i = 0; i < img->width * img->height; i++) img->pixels[i] = map[img->pixels[i]];
}

/* ---- setup ---- */
static int load_classes(Game *g, const char *dir)
{
    ResFile rf;
    char err[256], msg[256];
    if (rsrc_open(&rf, dir, "Apeiron Sprites", err, sizeof err)) { snprintf(g->error, sizeof g->error, "%s", err); return -1; }
    for (int k = 0; k < GAME_CLASSES; k++) {
        const Resource *r = rsrc_find(&rf, "PICT", kClasses[k].pict);
        PictImage img;
        if (!r || pict_decode(r->data, r->size, &img, msg, sizeof msg) != PICT_OK || !img.indexed) {
            snprintf(g->error, sizeof g->error, "Apeiron Sprites: PICT %d missing or unreadable", kClasses[k].pict);
            rsrc_close(&rf);
            return -1;
        }
        remap_image(g, &img);
        GameClass *c = &g->cls[k];
        c->w = kClasses[k].w; c->h = kClasses[k].h; c->frames = kClasses[k].frames;
        c->pixels = (uint8_t *)calloc((size_t)c->frames * c->w * c->h, 1);
        for (int f = 0; f < c->frames; f++)
            for (int y = 0; y < c->h; y++)
                for (int x = 0; x < c->w; x++) {
                    int sx = kClasses[k].x0 + f * kClasses[k].pitch + x, sy = kClasses[k].y0 + y;
                    if (sx < img.width && sy < img.height) c->pixels[((size_t)f * c->h + y) * c->w + x] = img.pixels[sy * img.width + sx];
                }
        pict_free(&img);
        /* the class record the engine reads: handle -> data { frames, rect, offsets of the two tables, the tables } */
        uint32_t handle = CLASS_HANDLES + (uint32_t)k * 4, d = CLASS_DATA + (uint32_t)k * 0x1000;
        gm_wr32(&g->ctx, handle, d);
        gm_wr16(&g->ctx, d + 4, (uint16_t)c->frames);
        gm_wr16(&g->ctx, d + 6, 0); gm_wr16(&g->ctx, d + 8, 0);
        gm_wr16(&g->ctx, d + 0xA, (uint16_t)c->h); gm_wr16(&g->ctx, d + 0xC, (uint16_t)c->w);
        gm_wr32(&g->ctx, d + 0xE, 0x20);
        gm_wr32(&g->ctx, d + 0x12, 0x20 + (uint32_t)c->frames * 4);
        for (int f = 0; f < c->frames; f++) {
            uint32_t draw = BLIT_BASE | (uint32_t)k << 12 | (uint32_t)f << 1, copy = draw | 1;
            gm_wr32(&g->ctx, d + 0x20 + (uint32_t)f * 4, draw - d);
            gm_wr32(&g->ctx, d + 0x20 + (uint32_t)c->frames * 4 + (uint32_t)f * 4, copy - d);
        }
        w32(g, kClasses[k].a5, handle);
    }
    rsrc_close(&rf);
    return 0;
}

static void load_backdrops(Game *g, const char *dir)
{
    ResFile rf;
    char err[256], msg[256];
    if (rsrc_open(&rf, dir, "Apeiron Titles", err, sizeof err)) return;
    for (int i = 0; i < 11; i++) {
        const Resource *r = rsrc_find(&rf, "ppat", 128 + i);
        PictImage img;
        if (!r || ppat_decode(r->data, r->size, &img, msg, sizeof msg) != 0) continue;
        remap_image(g, &img);
        g->backdrop_w[i] = img.width; g->backdrop_h[i] = img.height;
        g->backdrop[i] = (uint8_t *)malloc((size_t)img.width * img.height);
        if (g->backdrop[i]) memcpy(g->backdrop[i], img.pixels, (size_t)img.width * img.height);
        pict_free(&img);
    }
    rsrc_close(&rf);
}

/* seg3_32F8: fill the saved background with the level's pattern (128 + the cycle number), tiled from the top left */
static void fill_backdrop(Game *g)
{
    int cycle = (int16_t)gm_rd16(&g->ctx, a5(g, -0x6E0));
    if (cycle < 0 || cycle > 10 || !g->backdrop[cycle]) return;
    const uint8_t *px = g->backdrop[cycle];
    int w = g->backdrop_w[cycle], h = g->backdrop_h[cycle];
    for (int y = 0; y < GAME_SCREEN_H; y++)
        for (int x = 0; x < GAME_FIELD_W; x++) g->mem[SAVED + (size_t)y * GAME_FIELD_W + x] = px[(y % h) * w + (x % w)];
}

static int load_jump_table(Game *g, const char *dir)
{
    ResFile rf;
    char err[256];
    if (rsrc_open(&rf, dir, "Apeiron", err, sizeof err)) { snprintf(g->error, sizeof g->error, "the Apeiron application file is needed: %s", err); return -1; }
    const Resource *r = rsrc_find(&rf, "CODE", 0);
    if (!r || r->size < 16) { snprintf(g->error, sizeof g->error, "Apeiron: no CODE 0 resource"); rsrc_close(&rf); return -1; }
    const uint8_t *c0 = r->data;
    uint32_t below = be32(c0 + 4), jt_off = be32(c0 + 12);
    g->ctx.a5 = A5_WORLD + below;
    for (uint32_t i = 0; i < (r->size - 16) / 8; i++) {
        uint32_t off = (uint32_t)(c0[16 + i * 8] << 8 | c0[17 + i * 8]), seg = (uint32_t)(c0[20 + i * 8] << 8 | c0[21 + i * 8]);
        uint32_t target = CODE_BASE + (seg - 1) * SEG_STRIDE + 4 + off;
        uint32_t p = A5_WORLD + below + jt_off + i * 8;
        gm_wr16(&g->ctx, p + 2, 0x4EF9); gm_wr32(&g->ctx, p + 4, target);       /* a jsr d(A5) lands at entry + 2 */
    }
    rsrc_close(&rf);
    return 0;
}

int game_init(Game *g, const char *dir)
{
    memset(g, 0, sizeof *g);
    snprintf(g->data_dir, sizeof g->data_dir, "%s", dir);
    g->mem = (uint8_t *)calloc(MEM_SIZE_BYTES, 1);
    if (!g->mem) { snprintf(g->error, sizeof g->error, "out of memory"); return -1; }
    g->ctx.mem = g->mem; g->ctx.ext = game_ext; g->ctx.ud = g;
    g->ctx.native_overlap = 1; g->ctx.reuse_memory = 1; g->ctx.heap_next = HEAP;
    if (load_jump_table(g, dir) || load_palette(g, dir) || load_classes(g, dir)) return -1;
    {
        ResFile prf; char perr[256], pmsg[256];
        if (!rsrc_open(&prf, dir, "Apeiron Titles", perr, sizeof perr)) {
            const Resource *pr = rsrc_find(&prf, "PICT", 3130);
            if (pr && pict_decode(pr->data, pr->size, &g->pause_pic, pmsg, sizeof pmsg) != PICT_OK) memset(&g->pause_pic, 0, sizeof g->pause_pic);
            rsrc_close(&prf);
        }
    }

    /* off-screen buffers and the row tables the engine draws through */
    g->screen_addr = SCREEN;
    memset(g->mem + SCREEN, g->black_index, GAME_SCREEN_W * GAME_SCREEN_H);
    memset(g->mem + WORK, g->black_index, GAME_FIELD_W * GAME_SCREEN_H);
    memset(g->mem + SAVED, g->black_index, GAME_FIELD_W * GAME_SCREEN_H);
    for (int y = 0; y < GAME_SCREEN_H; y++) {
        gm_wr32(&g->ctx, ROWS_SCREEN + (uint32_t)y * 4, SCREEN + (uint32_t)y * GAME_SCREEN_W);
        gm_wr32(&g->ctx, ROWS_WORK + (uint32_t)y * 4, WORK + (uint32_t)y * GAME_FIELD_W);
        gm_wr32(&g->ctx, ROWS_SAVED + (uint32_t)y * 4, SAVED + (uint32_t)y * GAME_FIELD_W);
    }
    /* the side panel */
    {
        ResFile rf; char err[256], msg[256]; PictImage img;
        if (!rsrc_open(&rf, dir, "Apeiron Titles", err, sizeof err)) {
            const Resource *r = rsrc_find(&rf, "PICT", 401);
            if (r && pict_decode(r->data, r->size, &img, msg, sizeof msg) == PICT_OK && img.indexed) {
                remap_image(g, &img);
                for (int y = 0; y < img.height && y < GAME_SCREEN_H; y++)
                    for (int x = 0; x < img.width && GAME_SCREEN_W - img.width + x < GAME_SCREEN_W; x++)
                        g->mem[SCREEN + (uint32_t)y * GAME_SCREEN_W + (GAME_SCREEN_W - img.width) + x] = img.pixels[y * img.width + x];
                pict_free(&img);
            }
            rsrc_close(&rf);
        }
    }

    /* the sprite engine, its five lists, the grids and the application globals */
    GameCtx *c = &g->ctx;
    w8(g, -0x75A, 1); w32(g, -0x758, ENGINE);
    gm_wr32(c, ENGINE + 0x14, BOUNDS);
    gm_wr16(c, BOUNDS + 0x14, GAME_SCREEN_H); gm_wr16(c, BOUNDS + 0x16, GAME_FIELD_W);
    gm_wr32(c, ENGINE + 0x18, ROWS_SCREEN); gm_wr32(c, ENGINE + 0x1C, ROWS_WORK); gm_wr32(c, ENGINE + 0x20, ROWS_SAVED);
    gm_wr16(c, ENGINE + 0x24, GAME_SCREEN_W); gm_wr16(c, ENGINE + 0x26, GAME_FIELD_W); gm_wr16(c, ENGINE + 0x28, GAME_FIELD_W);
    gm_wr16(c, ENGINE + 0x2A, 2);
    w32(g, -0x644, engine_list_create(c, 0x706C6179u));      /* play */
    w32(g, -0x648, engine_list_create(c, 0x70656465u));      /* pede */
    uint32_t shrm = engine_list_create(c, 0x7368726Du);      /* shrm: not moved */
    w32(g, -0x64C, shrm); gm_wr8(c, shrm + 0xC, 0);
    w32(g, -0x640, engine_list_create(c, 0x78747261u));      /* xtra */
    w32(g, -0x63C, engine_list_create(c, 0x6578706Cu));      /* expl */
    w32(g, -0x6F0, GRID); w32(g, -0x6EC, GRID2);
    /* limits of the shooter's box, the pede's field and the shots */
    w16(g, -0x654, 0); w16(g, -0x650, 0x1D0); w16(g, -0x652, 0); w16(g, -0x64E, 0x1A0);
    w16(g, -0x662, 0); w16(g, -0x65E, 0x1A0); w16(g, -0x664, 0x1D0 - 96); w16(g, -0x660, 0x1D0);
    w16(g, -0x742, GAME_FIELD_W); w16(g, -0x746, 0); w16(g, -0x744, GAME_SCREEN_H); w16(g, -0x748, 0);
    gm_wr32(c, 0xA01C2u, (uint32_t)time(NULL) | 1u);            /* seed of the random number generator (the original seeds it from the clock) */
    w16(g, -0x698, GAME_SCREEN_W);
    w32(g, -0x69C, SCREEN + 133 * GAME_SCREEN_W + 597);      /* score: right end of the number */
    w32(g, -0x678, SCREEN + 233 * GAME_SCREEN_W + 597);      /* bonus */
    w32(g, -0x6A0, SCREEN + 432 * GAME_SCREEN_W + 540);      /* wave */
    w32(g, -0x68A, SCREEN + 340 * GAME_SCREEN_W + 597);      /* life icons */
    w32(g, -0x668, SCREEN + 381 * GAME_SCREEN_W + 492);      /* power-up icons */
    load_backdrops(g, dir);
    load_hud_art(g, dir);
    load_sounds(g, dir);
    g->running = 1;
    return 0;
}

void game_dump_class(const Game *g, int k, const char *path)       /* all frames of a class side by side, as a PPM, for checking colours */
{
    const GameClass *c = &g->cls[k];
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int per_row = 16, rows = (c->frames + per_row - 1) / per_row, cw = (c->w + 1) * 4, ch = (c->h + 1) * 4;
    fprintf(f, "P6 %d %d 255 ", per_row * cw, rows * ch);
    for (int y = 0; y < rows * ch; y++)
        for (int x = 0; x < per_row * cw; x++) {
            int fi = (y / ch) * per_row + x / cw, px = (x % cw) / 4, py = (y % ch) / 4;
            uint8_t rgb[3] = { 40, 40, 40 };
            if (fi < c->frames && px < c->w && py < c->h) {
                uint8_t v = c->pixels[((size_t)fi * c->h + py) * c->w + px];
                if (v) memcpy(rgb, &g->palette[v * 3], 3);
            }
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

void game_dump_class_raw(const Game *g, int k, const char *path)   /* the raw palette indices of every frame */
{
    const GameClass *c = &g->cls[k];
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(c->pixels, 1, (size_t)c->frames * c->w * c->h, f);
    fclose(f);
}

void game_free(Game *g)
{
    pict_free(&g->pause_pic);
    for (int k = 0; k < GAME_CLASSES; k++) free(g->cls[k].pixels);
    for (int k = 0; k < 7; k++) free(g->icon[k]);
    for (int k = 0; k < 11; k++) free(g->backdrop[k]);
    mixer_free(&g->mixer);
    free(g->mem);
    memset(g, 0, sizeof *g);
}

/* The colour tables of the original (seg2_049C): handles to Color Tables in the memory image, so the ported code can read them as it does the original's.
 * -0x4C4 is the game's table; -0x728, -0x724, -0x720 are the three flash tables made by seg2_1974: each is a copy of the one before (the first of the
 * game's table) with every entry's red, green and blue rotated (red <- green, green <- blue, blue <- red). */
static int in_tables(uint32_t p) { return p >= CTAB_BASE && p < CTAB_BASE + 5 * CTAB_SIZE; }

/* The Toolbox's fixed-point routines, as seg6_0C80 uses them */
static int32_t fix_ratio(int16_t numer, int16_t denom)          /* only the case that occurs matters: a zero denominator */
{
    if (denom == 0) return numer < 0 ? (int32_t)0x80000000 : 0x7FFFFFFF;
    int64_t r = ((int64_t)numer << 16) / denom;
    return r > 0x7FFFFFFF ? 0x7FFFFFFF : r < -0x7FFFFFFFLL - 1 ? (int32_t)0x80000000 : (int32_t)r;
}
static int32_t long2fix(uint32_t x) { return x > 0x7FFF ? 0x7FFFFFFF : (int32_t)(x << 16); }     /* saturates (the value 0xFFFF must not wrap) */
static int32_t fix_mul(int32_t a, int32_t b)
{
    int64_t r = ((int64_t)a * b) >> 16;
    return r > 0x7FFFFFFF ? 0x7FFFFFFF : r < -0x7FFFFFFFLL - 1 ? (int32_t)0x80000000 : (int32_t)r;
}

static void build_palette_tables(Game *g)
{
    GameCtx *c = &g->ctx;
    uint8_t cur[256 * 3];
    memcpy(g->base_palette, g->palette, sizeof cur);
    memcpy(cur, g->palette, sizeof cur);
    static const int slot[4] = { -0x4C4, -0x728, -0x724, -0x720 };
    for (int k = 0; k < 4; k++) {
        if (k > 0) for (int i = 0; i < 256; i++) { uint8_t r = cur[i * 3], gg = cur[i * 3 + 1], b = cur[i * 3 + 2]; cur[i * 3] = gg; cur[i * 3 + 1] = b; cur[i * 3 + 2] = r; }
        uint32_t cell = CTAB_BASE + (uint32_t)k * CTAB_SIZE, ct = cell + 16;
        gm_wr32(c, cell, ct);
        gm_wr32(c, ct, 0); gm_wr16(c, ct + 4, 0x8000); gm_wr16(c, ct + 6, 255);
        for (int i = 0; i < 256; i++) {
            gm_wr16(c, ct + 8 + (uint32_t)i * 8, (uint16_t)i);
            for (int ch = 0; ch < 3; ch++) gm_wr16(c, ct + 10 + (uint32_t)i * 8 + (uint32_t)ch * 2, (uint16_t)(cur[i * 3 + ch] * 257));
        }
        w32(g, slot[k], cell);
    }
    /* -0x71C, the death flash (seg2_049C: seg6_0B00(1, 0) then a copy of the table): seg6_0C80 with the colour red (0xFFFF, 0, 0). For each entry the ratio
     * FixRatio(lightness of the entry, lightness of red) scales the colour, and the table gets the integer part of the result: the picture in shades of red,
     * its brightness following the lightness of the original colour (this matches the user's screenshots of the real flash) */
    {
        static const uint16_t red[3] = { 0xFFFF, 0, 0 };
        uint32_t cell = CTAB_BASE + 4u * CTAB_SIZE, ct = cell + 16;
        gm_wr32(c, cell, ct);
        gm_wr32(c, ct, 0); gm_wr16(c, ct + 4, 0x8000); gm_wr16(c, ct + 6, 255);
        for (int i = 0; i < 256; i++) {
            unsigned mx = g->base_palette[i * 3], mn = mx;
            for (int ch = 1; ch < 3; ch++) { unsigned v = g->base_palette[i * 3 + ch]; if (v > mx) mx = v; if (v < mn) mn = v; }
            int32_t ratio = fix_ratio((int16_t)((mx + mn) * 257 / 2), (int16_t)0x7FFF);       /* lightness of the entry over the lightness of red (seg6_0C80 asks RGB2HSL, trap A82E selector 6) */
            gm_wr16(c, ct + 8 + (uint32_t)i * 8, (uint16_t)i);
            for (int ch = 0; ch < 3; ch++) gm_wr16(c, ct + 10 + (uint32_t)i * 8 + (uint32_t)ch * 2, (uint16_t)((uint32_t)fix_mul(ratio, long2fix(red[ch])) >> 16));
        }
        w32(g, -0x71C, cell);
    }
}

static void ext_music_start(Game *g)
{
    if (g->audio_lock) g->audio_lock(g->audio_ud, 1);
    mixer_music_start(&g->mixer, 1000, 1001);
    if (g->audio_lock) g->audio_lock(g->audio_ud, 0);
}

void game_new(Game *g)
{
    build_palette_tables(g);
    if (gm_rd32(&g->ctx, 0x16A) == 0) gm_wr32(&g->ctx, 0x16A, 36000u);       /* Ticks as the original sees it: counting from boot */
    /* seg3_0E1A, the original's new-game routine: these are its assignments, in its order */
    GameCtx *c = &g->ctx;
    w8(g, -0x45F, 2);                              /* the mode byte: playing (the pause sets 3) */
    w8(g, -0x729, 0); w32(g, -0x6DE, 0);
    w32(g, -0x6DA, gm_rd32(c, 0x16A)); w8(g, -0x715, 0);
    w16(g, -0x734, getenv("APEIRON_LEVEL") ? (uint16_t)atoi(getenv("APEIRON_LEVEL")) : 1); w16(g, -0x732, 0); w8(g, -0x730, 1);      /* APEIRON_LEVEL: test, start at that wave */
    w32(g, -0x702, 0); w16(g, -0x674, 1); w32(g, -0x6FE, 0xFFFFFFFFu);
    w16(g, -0x6BC, 5); w16(g, -0x6A2, 0x12);
    w16(g, -0x6FA, 3); w16(g, -0x6F8, 0);
    w32(g, -0x696, gm_rd32(c, a5(g, -0x6DE))); w8(g, -0x6F6, 0); w32(g, -0x6F4, gm_rd32(c, a5(g, -0x6DE)));
    w32(g, -0x692, gm_rd32(c, a5(g, -0x6DE))); w32(g, -0x68E, 0); w8(g, -0x716, 0);
    w32(g, -0x6D6, 0xF0);
    w8(g, -0x66D, 0); w8(g, -0x66C, 0); w8(g, -0x66B, 0); w8(g, -0x66A, 0); w8(g, -0x669, 0);
    w8(g, -0x66E, 1);
    w32(g, -0x6C4, 0); w16(g, -0x6CA, 1);
    w8(g, -0x4D2, 1);                              /* the loop's "keep going" flag (set by the original's event loop) */
    if (gm_rd8(c, a5(g, -0x708))) ext_music_start(g);              /* seg3_4123C begins with seg3_1686: the music starts with the game */
    clear_playfield(c);                            /* seg3_30F6 */
    build_round(c);                                /* seg3_299A */
    engine_move_pass(c);                           /* seg3_00EA */
    engine_full_redraw(c);                         /* seg3_33A8: the first full draw (the backdrop is already in the saved buffer) */
    g->running = 1;
}

int game_scores_save(Game *g)
{
    ResFile rf;
    char err[256];
    if (!g->scores_ok || rsrc_open(&rf, g->data_dir, "Apeiron Scores", err, sizeof err)) return 1;
    const Resource *r = rsrc_find(&rf, "ApPr", 128);
    int rc = r ? rsrc_patch(&rf, r, g->scores.raw) : 1;
    rsrc_close(&rf);
    return rc;
}

void game_key(Game *g, unsigned char ch)
{
    if (gm_rd8(&g->ctx, a5(g, -0x45F)) == 3) cheat_key(&g->ctx, ch, g->cheat_codes);
}

int game_step(Game *g, int dx, int dy, int button, uint32_t ticks)
{
    GameCtx *c = &g->ctx;
    gm_wr32(c, 0x16A, ticks);
    w16(g, -0x41C, (uint16_t)((int16_t)gm_rd16(c, a5(g, -0x41C)) + dx));
    w16(g, -0x41A, (uint16_t)((int16_t)gm_rd16(c, a5(g, -0x41A)) + dy));
    w8(g, -0x418, button ? 1 : 0);
    g->n_sounds = 0;
    if (!game_pass(c)) g->running = 0;
    { static int watch = -1; static uint32_t pass; if (watch < 0) watch = getenv("APEIRON_PEDEWATCH") != NULL || pede_log_on(); if (watch) watch_pedes(c, pass++); }
    return g->running;
}

void game_render(const Game *g, uint32_t *fb)
{
    const uint8_t *s = g->mem + g->screen_addr;
    for (int i = 0; i < GAME_SCREEN_W * GAME_SCREEN_H; i++) {
        const uint8_t *p = &g->palette[s[i] * 3];
        fb[i] = 0xFF000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    }
    if (g->paused && g->pause_pic.pixels && g->pause_pic.indexed) {            /* seg4_085C: the picture sits horizontally centred and a third of the way down the window */
        const PictImage *im = &g->pause_pic;
        int ox = (GAME_SCREEN_W - im->width) / 2, oy = (GAME_SCREEN_H - im->height) / 3;
        for (int y = 0; y < im->height; y++)
            for (int x = 0; x < im->width; x++) {
                const uint8_t *c = &im->palette[im->pixels[y * im->width + x] * 3];
                fb[(oy + y) * GAME_SCREEN_W + ox + x] = 0xFF000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
            }
    }
}
