#include <stdio.h>
#include <stdlib.h>
#include "creatures.h"

#include "rng.h"

#define SEED_ADDR 0xA01C2u   /* the generator's seed lives in CODE 9 */

/* ---- memory ---- */
uint8_t gm_rd8(const GameCtx *g, uint32_t a) { return g->mem[a & MEM_MASK]; }
uint16_t gm_rd16(const GameCtx *g, uint32_t a) { a &= MEM_MASK; return (uint16_t)(g->mem[a] << 8 | g->mem[(a + 1) & MEM_MASK]); }
uint32_t gm_rd32(const GameCtx *g, uint32_t a) { return (uint32_t)gm_rd16(g, a) << 16 | gm_rd16(g, a + 2); }
void gm_wr8(GameCtx *g, uint32_t a, uint8_t v) { g->mem[a & MEM_MASK] = v; }
void gm_wr16(GameCtx *g, uint32_t a, uint16_t v) { gm_wr8(g, a, (uint8_t)(v >> 8)); gm_wr8(g, a + 1, (uint8_t)v); }
void gm_wr32(GameCtx *g, uint32_t a, uint32_t v) { gm_wr16(g, a, (uint16_t)(v >> 16)); gm_wr16(g, a + 2, (uint16_t)v); }

/* ---- sprite record fields ---- */
enum {
    F_W = 0x18, F_H = 0x1A,           /* size (int16) */
    F_VISIBLE = 0x1C, F_DIRTY = 0x1D, F_HIDDEN = 0x1E, F_REMOVE = 0x1F,
    F_POS_H = 0x2A, F_POS_V = 0x2E,   /* 16.16 */
    F_VEL_H = 0x3A, F_VEL_V = 0x3E,   /* 16.16 */
    F_FRAMES = 0x46, F_FRAME = 0x48, F_SCRATCH = 0x52
};

/* ---- application globals (offsets from A5) ---- */
enum {
    G_GRID = -0x6F0, G_TICKS = -0x6DE, G_SCORE = -0x702, G_SCORPION_TIMER = -0x6A6, G_FLEA_TIMER = -0x6BA,
    G_FLEA_COUNT = -0x6BE, G_MUSH_COUNT = -0x6E4, G_FIELD_MIN_V = -0x6A2, G_FIELD_H = -0x4B6, G_RESPAWN_V = -0x654,
    G_EXPL_VALUE = -0x610, G_PLAYFIELD = -0x758,
    S_GECKO = -0x51C, S_SCORP_A = -0x558, S_SCORP_B = -0x554, S_SHIP = -0x514, S_LEAVE = -0x548, S_BOUNCE = -0x524
};

#define A5G(g, off) ((g)->a5 + (uint32_t)(int32_t)(off))

static int32_t asr16(uint32_t v) { return (int32_t)v >> 16; }               /* moveq #16 / asr.l */
static int16_t w16(int32_t v) { return (int16_t)v; }
static uint32_t shl16(int32_t v) { return (uint32_t)v << 16; }

static uint32_t ext(GameCtx *g, int kind, const uint8_t *raw, int n) { return g->ext ? g->ext(g->ud, kind, raw, n) : 0; }

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)(v >> 16)); put16(p + 2, (uint16_t)v); }

static void sound(GameCtx *g, uint32_t id, uint16_t priority)
{
    uint8_t raw[10];
    put32(raw, id); put16(raw + 4, priority); put32(raw + 6, 0);
    ext(g, EXT_SOUND, raw, 10);
}

static uint16_t rnd(GameCtx *g, uint16_t n)
{
    uint32_t seed = gm_rd32(g, SEED_ADDR);
    uint16_t r = rng_random(&seed, n, 0);
    gm_wr32(g, SEED_ADDR, seed);
    return r;
}

/* address of grid cell [h][v] as the original computes it: h*0x78 + base + v*4 (no bounds check) */
static uint32_t cell(GameCtx *g, int16_t h, int16_t v)
{
    return (uint32_t)((int32_t)h * 0x78 + (int32_t)gm_rd32(g, A5G(g, G_GRID)) + (int32_t)v * 4);
}

/* ---- engine ---- */
void sprite_deactivate(GameCtx *g, uint32_t s)
{
    if (gm_rd8(g, s + F_VISIBLE)) {
        gm_wr8(g, s + F_VISIBLE, 0);
        gm_wr8(g, s + F_HIDDEN, 1);
        gm_wr8(g, s + F_DIRTY, 0);
    }
}

void grid_remove_mushroom(GameCtx *g, int16_t h, int16_t v)
{
    uint32_t m = gm_rd32(g, cell(g, h, v));
    if (m) {
        sprite_deactivate(g, m);
        gm_wr8(g, m + F_REMOVE, 1);
        if (v >= 0x12) gm_wr16(g, A5G(g, G_MUSH_COUNT), (uint16_t)(gm_rd16(g, A5G(g, G_MUSH_COUNT)) - 1));
        gm_wr32(g, cell(g, h, v), 0);
    }
}

void sprite_move_default(GameCtx *g, uint32_t s)
{
    gm_wr32(g, s + F_POS_H, gm_rd32(g, s + F_POS_H) + gm_rd32(g, s + F_VEL_H));
    int16_t d7 = (int16_t)gm_rd16(g, s + F_W);
    uint32_t rect = gm_rd32(g, gm_rd32(g, A5G(g, G_PLAYFIELD)) + 0x14);
    int16_t d6 = (int16_t)gm_rd16(g, rect + 0x16);
    int32_t d0 = asr16(gm_rd32(g, s + F_POS_H)) + d7;
    if (d6 < d0) {
        gm_wr32(g, s + F_POS_H, shl16((int32_t)d6 - d7));
        gm_wr32(g, s + F_VEL_H, 0u - gm_rd32(g, s + F_VEL_H));
    }
    if (asr16(gm_rd32(g, s + F_POS_H)) < 0) {
        gm_wr32(g, s + F_POS_H, 0);
        gm_wr32(g, s + F_VEL_H, 0u - gm_rd32(g, s + F_VEL_H));
    }
    gm_wr32(g, s + F_POS_V, gm_rd32(g, s + F_POS_V) + gm_rd32(g, s + F_VEL_V));
    d7 = (int16_t)gm_rd16(g, s + F_H);
    rect = gm_rd32(g, gm_rd32(g, A5G(g, G_PLAYFIELD)) + 0x14);
    d6 = (int16_t)gm_rd16(g, rect + 0x14);
    d0 = asr16(gm_rd32(g, s + F_POS_V)) + d7;
    if (d6 < d0) {
        gm_wr32(g, s + F_POS_V, shl16((int32_t)d6 - d7));
        gm_wr32(g, s + F_VEL_V, 0u - gm_rd32(g, s + F_VEL_V));
    }
    if (asr16(gm_rd32(g, s + F_POS_V)) < 0) {
        gm_wr32(g, s + F_POS_V, 0);
        gm_wr32(g, s + F_VEL_V, 0u - gm_rd32(g, s + F_VEL_V));
    }
}

/* ---- frame callbacks ---- */
void frame_next_to_count(GameCtx *g, uint32_t s)
{
    int16_t f = (int16_t)(gm_rd16(g, s + F_FRAME) + 1);
    if (f > (int16_t)gm_rd16(g, s + F_FRAMES)) f = 1;
    gm_wr16(g, s + F_FRAME, (uint16_t)f);
}

static void frame_cycle(GameCtx *g, uint32_t s, int16_t step, int16_t last, int16_t reset)
{
    int16_t f = (int16_t)(gm_rd16(g, s + F_FRAME) + step);
    if (f > last) f = reset;
    gm_wr16(g, s + F_FRAME, (uint16_t)f);
}

void frame_gecko(GameCtx *g, uint32_t s) { frame_cycle(g, s, 1, 8, 1); }
void frame_scorpion(GameCtx *g, uint32_t s) { frame_cycle(g, s, 1, 0x14, 1); }

void frame_psychedelic(GameCtx *g, uint32_t s)
{
    int16_t f = (int16_t)(gm_rd16(g, s + F_FRAME) + 4);
    if (f > (int16_t)gm_rd16(g, s + F_FRAMES)) f = 5;
    gm_wr16(g, s + F_FRAME, (uint16_t)f);
}

void frame_pede_body(GameCtx *g, uint32_t s)
{
    int16_t base = 0, count = 0;
    uint32_t rec = gm_rd32(g, s + F_SCRATCH);
    int8_t dir = (int8_t)gm_rd8(g, rec + 0x84);
    if (dir > 0) {
        switch (dir) {
        case 1: base = 0xF; count = 0xE; break;
        case 2: base = 1; count = 0xE; break;
        case 3: case 4: base = 0x1D; count = 3; break;
        default: break;                      /* 5.. : stays 0 */
        }
    }
    if (gm_rd32(g, s) == 0x70626F64u) base = (int16_t)(base + 0x1F);   /* 'dobp': the head */
    int16_t f = (int16_t)(gm_rd16(g, rec + 0x88) + 1);
    if (count < f) f = 1;
    gm_wr16(g, rec + 0x88, (uint16_t)f);
    gm_wr16(g, s + F_FRAME, (uint16_t)(base + f - 1));
}

/* ---- creatures ---- */
void move_gecko(GameCtx *g, uint32_t s)
{
    gm_wr32(g, s + F_POS_H, gm_rd32(g, s + F_POS_H) - gm_rd32(g, s + F_VEL_H));
    int32_t d7 = asr16(gm_rd32(g, s + F_POS_H));
    int32_t d6 = asr16(gm_rd32(g, s + F_POS_V));
    int16_t row = (int16_t)(w16(10 + w16(d7)) >> 4);
    int16_t col = (int16_t)(w16(10 + w16(d6)) >> 4);
    uint32_t m = gm_rd32(g, cell(g, row, col));
    if (m && (int16_t)gm_rd16(g, m + F_FRAME) > 4) {
        if (getenv("APEIRON_WATCH_MUSH")) fprintf(stderr, "GECKO poisons mushroom %06X at %d,%d (frame was %d)%c", (unsigned)m, asr16(gm_rd32(g, m + F_POS_H)), asr16(gm_rd32(g, m + F_POS_V)), (int)(int16_t)gm_rd16(g, m + F_FRAME), 10);
        gm_wr16(g, m + F_FRAME, 1);
        gm_wr8(g, m + F_DIRTY, 1);
        gm_wr8(g, m + F_HIDDEN, 1);
        sound(g, gm_rd32(g, A5G(g, S_GECKO)), 2);
    }
    if (w16(d7) <= 0) {
        sprite_deactivate(g, s);
        gm_wr32(g, s + F_POS_H, 0);
    }
}

static void scorpion_respawn_timer(GameCtx *g)
{
    int16_t r = (int16_t)rnd(g, 0x168);
    gm_wr32(g, A5G(g, G_SCORPION_TIMER), gm_rd32(g, A5G(g, G_TICKS)) + (uint32_t)(int32_t)r + 0xB4u);
}

void move_scorpion(GameCtx *g, uint32_t s)
{
    int16_t odds = (int32_t)gm_rd32(g, A5G(g, G_SCORE)) < 0x1388 ? 0x1E : 0x12;
    if (rnd(g, (uint16_t)odds) == 1) {
        if (rnd(g, 2) == 0) gm_wr32(g, s + F_VEL_V, 0u - gm_rd32(g, s + F_VEL_V));
        if (rnd(g, 4) == 0) gm_wr32(g, s + F_SCRATCH, 1); else gm_wr32(g, s + F_SCRATCH, 0);
    }
    if (gm_rd32(g, s + F_SCRATCH) == 0) gm_wr32(g, s + F_POS_H, gm_rd32(g, s + F_POS_H) + gm_rd32(g, s + F_VEL_H));
    gm_wr32(g, s + F_POS_V, gm_rd32(g, s + F_POS_V) + gm_rd32(g, s + F_VEL_V));
    int32_t d7 = asr16(gm_rd32(g, s + F_POS_H));
    int32_t d6 = asr16(gm_rd32(g, s + F_POS_V));
    if (w16(d7) < 0) {
        sprite_deactivate(g, s);
        gm_wr32(g, s + F_POS_H, 0);
        scorpion_respawn_timer(g);
    }
    if (w16((int32_t)gm_rd16(g, s + F_W) + w16(d7)) >= 0x1B0) {
        sprite_deactivate(g, s);
        gm_wr32(g, s + F_POS_H, shl16(w16((int32_t)gm_rd16(g, A5G(g, G_FIELD_H)) - (int16_t)gm_rd16(g, s + F_H))));
        scorpion_respawn_timer(g);
    }
    int16_t d5 = (int16_t)(gm_rd16(g, A5G(g, G_FIELD_MIN_V)) << 4);
    if (d5 > w16(d6)) {
        gm_wr32(g, s + F_VEL_V, 0u - gm_rd32(g, s + F_VEL_V));
        gm_wr32(g, s + F_POS_V, shl16(d5));
    }
    if (w16((int32_t)gm_rd16(g, s + F_H) + w16(d6)) > 0x1E0) {
        gm_wr32(g, s + F_VEL_V, 0u - gm_rd32(g, s + F_VEL_V));
        gm_wr32(g, s + F_POS_V, shl16(w16(0x1E0 - (int16_t)gm_rd16(g, s + F_H))));
    }
    int16_t row = (int16_t)(w16(0xC + w16(d7)) >> 4);
    int16_t col = (int16_t)(w16(0xC + w16(d6)) >> 4);
    if (gm_rd32(g, cell(g, row, col)) != 0) {
        grid_remove_mushroom(g, row, col);
        if (rnd(g, 2) == 0) sound(g, gm_rd32(g, A5G(g, S_SCORP_A)), 2);
        else sound(g, gm_rd32(g, A5G(g, S_SCORP_B)), 2);
    }
}

void move_flea(GameCtx *g, uint32_t s)
{
    if (gm_rd32(g, s + F_VEL_H) != 0) {
        gm_wr32(g, s + F_VEL_V, gm_rd32(g, s + F_VEL_V) + 0x8000u);
        if (w16(asr16(gm_rd32(g, s + F_VEL_V))) > 0x10) gm_wr32(g, s + F_VEL_V, 0x100000u);
    }
    gm_wr32(g, s + F_POS_V, gm_rd32(g, s + F_POS_V) + gm_rd32(g, s + F_VEL_V));
    gm_wr32(g, s + F_POS_H, gm_rd32(g, s + F_POS_H) + gm_rd32(g, s + F_VEL_H));
    int32_t d7 = asr16(gm_rd32(g, s + F_POS_V));
    int32_t d6 = asr16(gm_rd32(g, s + F_POS_H));
    int16_t d5 = (int16_t)(w16(d7) >> 4);
    if ((int32_t)d5 != (int32_t)(gm_rd32(g, s + F_SCRATCH) & 0xFFFF)) {
        gm_wr32(g, s + F_SCRATCH, (gm_rd32(g, s + F_SCRATCH) & 0xFFFF0000u) + (uint32_t)(int32_t)d5);
        if (rnd(g, 4) == 0 && w16(w16(d7) >> 4) < 0x1D) {
            sound(g, gm_rd32(g, A5G(g, S_GECKO)), 1);
            uint8_t raw[4];
            put16(raw, (uint16_t)(w16(d6) >> 4)); put16(raw + 2, (uint16_t)d5);
            ext(g, EXT_MUSHROOM, raw, 4);
            if (rnd(g, 10) == 0 && gm_rd32(g, s + F_VEL_H) == 0) gm_wr32(g, s + F_VEL_V, 0x80000u);
        }
    }
    if (w16((int32_t)gm_rd16(g, s + F_H) + w16(d7)) > 0x1E0) {
        if (gm_rd32(g, s + F_VEL_H) == 0) {
            sprite_deactivate(g, s);
            gm_wr16(g, A5G(g, G_FLEA_COUNT), (uint16_t)(gm_rd16(g, A5G(g, G_FLEA_COUNT)) - 1));
            gm_wr8(g, s + F_REMOVE, 1);
            d7 = (d7 & ~0xFFFF) | gm_rd16(g, A5G(g, G_RESPAWN_V));
            int16_t r = (int16_t)rnd(g, 0x168);
            gm_wr32(g, A5G(g, G_FLEA_TIMER), gm_rd32(g, A5G(g, G_TICKS)) + 0x78u + (uint32_t)(int32_t)r);
        } else {
            int16_t vy = w16(asr16(gm_rd32(g, s + F_VEL_V)));
            if (vy < 10 && vy >= 0) {
                sprite_deactivate(g, s);
                gm_wr8(g, s + F_REMOVE, 1);
                gm_wr16(g, A5G(g, G_FLEA_COUNT), (uint16_t)(gm_rd16(g, A5G(g, G_FLEA_COUNT)) - 1));
            } else if (vy > 0) {
                vy = (int16_t)(vy - 2);
                gm_wr32(g, s + F_POS_V, 0x1D00000u);
                gm_wr32(g, s + F_VEL_V, shl16(-(int32_t)vy));
                sound(g, gm_rd32(g, A5G(g, S_BOUNCE)), 2);
            }
        }
    }
    if (w16(d7) < 0) gm_wr32(g, s + F_POS_V, 0);
    if (w16(d6) < 0) {
        gm_wr32(g, s + F_POS_H, 0);
        gm_wr32(g, s + F_VEL_H, 0u - gm_rd32(g, s + F_VEL_H));
        sound(g, gm_rd32(g, A5G(g, S_BOUNCE)), 2);
    }
    if (w16(d6) > 0x1A0) {
        gm_wr32(g, s + F_POS_H, 0x1A00000u);
        gm_wr32(g, s + F_VEL_H, 0u - gm_rd32(g, s + F_VEL_H));
        sound(g, gm_rd32(g, A5G(g, S_BOUNCE)), 2);
    }
}

static void ship_leaves(GameCtx *g, uint32_t s)
{
    sound(g, gm_rd32(g, A5G(g, S_LEAVE)), 3);
    sprite_deactivate(g, s);
    gm_wr8(g, s + F_REMOVE, 1);
}

void move_spaceship(GameCtx *g, uint32_t s)
{
    gm_wr32(g, s + F_POS_H, gm_rd32(g, s + F_POS_H) + gm_rd32(g, s + F_VEL_H));
    gm_wr32(g, s + F_POS_V, gm_rd32(g, s + F_POS_V) + gm_rd32(g, s + F_VEL_V));
    int32_t d7 = asr16(gm_rd32(g, s + F_POS_H));
    int32_t d6 = asr16(gm_rd32(g, s + F_POS_V));
    int16_t row = (int16_t)(w16(10 + w16(d7)) >> 4);
    int16_t col = (int16_t)(w16(10 + w16(d6)) >> 4);
    if (row < 0x1A) {
        uint32_t m = gm_rd32(g, cell(g, row, col));
        if (m) {
            uint8_t raw[8];
            put32(raw, m); put32(raw + 4, gm_rd32(g, A5G(g, G_EXPL_VALUE)));
            ext(g, EXT_EXPLOSION, raw, 8);
            grid_remove_mushroom(g, row, col);
            sound(g, gm_rd32(g, A5G(g, S_SHIP)), 2);
        }
    }
    if (rnd(g, 10) == 1) {
        uint32_t vel = 0;
        switch (rnd(g, 3)) {
        case 0: vel = 0xFFFD0000u; break;
        case 2: vel = 0x30000u; break;
        default: vel = 0; break;
        }
        gm_wr32(g, s + F_VEL_V, vel);
    }
    if (w16((int32_t)gm_rd16(g, s + F_W) + w16(d7)) > 0x1B0) ship_leaves(g, s);
    if (w16(d7) < 0) ship_leaves(g, s);
    if (w16(d6) < 0) {
        gm_wr32(g, s + F_POS_V, 0);
        gm_wr32(g, s + F_VEL_V, 0);
    }
    if (w16((int32_t)gm_rd16(g, s + F_H) + w16(d6)) > 0x1E0) {
        /* the original stores the plain pixel value here, not 16.16 */
        gm_wr32(g, s + F_POS_V, (uint32_t)(int32_t)w16(0x1E0 - (int16_t)gm_rd16(g, s + F_H)));
        gm_wr32(g, s + F_VEL_V, 0);
    }
}

void move_coin(GameCtx *g, uint32_t s)
{
    gm_wr16(g, s + F_VEL_V, (uint16_t)(gm_rd16(g, s + F_VEL_V) + 1));   /* word add on the high half of the 16.16 value */
    gm_wr32(g, s + F_POS_V, gm_rd32(g, s + F_POS_V) + gm_rd32(g, s + F_VEL_V));
    gm_wr32(g, s + F_POS_H, gm_rd32(g, s + F_POS_H) + gm_rd32(g, s + F_VEL_H));
    int32_t d7 = asr16(gm_rd32(g, s + F_POS_V));
    int32_t d6 = asr16(gm_rd32(g, s + F_POS_H));
    if (w16(d7) > 0x1D0) {
        gm_wr32(g, s + F_POS_V, 0);
        sprite_deactivate(g, s);
        gm_wr8(g, s + F_REMOVE, 1);
    }
    if (w16(d7) < 0) gm_wr32(g, s + F_POS_V, 0);
    if (w16(d6) < 0) {
        gm_wr32(g, s + F_POS_H, 0);
        gm_wr32(g, s + F_VEL_H, 0u - gm_rd32(g, s + F_VEL_H));
    }
    if (w16(d6) > 0x1A0) {
        gm_wr32(g, s + F_POS_H, 0x1A00000u);
        gm_wr32(g, s + F_VEL_H, 0u - gm_rd32(g, s + F_VEL_H));
    }
}

void move_timed_bonus(GameCtx *g, uint32_t s)
{
    uint32_t n = gm_rd32(g, s + F_SCRATCH) + 1;
    gm_wr32(g, s + F_SCRATCH, n);
    if (n == 0x12C) {
        sound(g, gm_rd32(g, A5G(g, S_LEAVE)), 3);
        sprite_deactivate(g, s);
        gm_wr8(g, s + F_REMOVE, 1);
    }
}

void move_bullet(GameCtx *g, uint32_t s)
{
    gm_wr32(g, s + F_POS_V, gm_rd32(g, s + F_POS_V) + gm_rd32(g, s + F_VEL_V));
    int32_t d7 = asr16(gm_rd32(g, s + F_POS_V));
    if (w16((int32_t)gm_rd16(g, s + F_H) + w16(d7)) >= 0x1E0) {
        sprite_deactivate(g, s);
        gm_wr32(g, s + F_POS_V, shl16((int16_t)gm_rd16(g, A5G(g, G_RESPAWN_V))));
    }
}

/* ---- Pentipede segments (seg3_4BC6) ---- */
enum {
    G_PEDE_GRID = -0x6EC, G_V_MAX = -0x650, G_H_MAX = -0x64E, G_H_MIN = -0x652, G_STEP = -0x6E8,
    G_TOP_LIMIT = -0x664, G_TOUCHED = -0x6D2, G_TOUCH_TICK = -0x6D0, G_PEDE_LIST = -0x648, S_POISON = -0x4F8
};
/* G_V_MIN (-0x654) is the same word as G_RESPAWN_V above. Fields of the per-segment record (sprite +0x52): */
enum { R_DIR = 0x84, R_NEXT = 0x85, R_TARGET = 0x86, R_AHEAD = 0x7C, R_BEHIND = 0x80, R_DIVE = 0x8A };
#define G_V_MIN G_RESPAWN_V
#define TAG_SHRM 0x7368726Du
#define TAG_PHED 0x70686564u
#define TAG_PBOD 0x70626F64u

static int32_t lmul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }   /* the library long multiply */

/* address of grid cell [h][v] for plain 32-bit cell numbers */
static uint32_t cell32(GameCtx *g, int grid_off, int32_t h, int32_t v)
{
    return (uint32_t)(lmul(h, 0x78) + (int32_t)gm_rd32(g, A5G(g, grid_off)) + (int32_t)((uint32_t)v << 2));
}

/* records the segment's state in its ring of 10 positions (12 bytes each: V, H, direction, next, target) */
static void ring_put(GameCtx *g, uint32_t rec, int32_t v, int32_t h)
{
    uint32_t i = (uint32_t)((int32_t)(int16_t)gm_rd16(g, rec + 2) * 12);
    gm_wr32(g, rec + 8 + i, (uint32_t)v);
    gm_wr32(g, rec + 4 + i, (uint32_t)h);
    gm_wr8(g, rec + 0xC + i, gm_rd8(g, rec + R_DIR));
    gm_wr8(g, rec + 0xD + i, gm_rd8(g, rec + R_NEXT));
    gm_wr16(g, rec + 0xE + i, gm_rd16(g, rec + R_TARGET));
    gm_wr16(g, rec + 2, (uint16_t)(gm_rd16(g, rec + 2) + 1));
    if ((int16_t)gm_rd16(g, rec + 2) >= 10) gm_wr16(g, rec + 2, 0);
}

/* the "next" direction becomes current; going down (4) the target is 16 below, otherwise 16 above */
static void turn(GameCtx *g, uint32_t rec, uint8_t next, int32_t v)
{
    gm_wr8(g, rec + R_DIR, gm_rd8(g, rec + R_NEXT));
    gm_wr8(g, rec + R_NEXT, next);
    if (gm_rd8(g, rec + R_DIR) == 4) gm_wr16(g, rec + R_TARGET, (uint16_t)(0x10 + (int16_t)v));
    else gm_wr16(g, rec + R_TARGET, (uint16_t)(-0x10 + (int16_t)v));
}

static int g_dump_on;
/* a mushroom in the way; poison (frame 1..4) makes the head dive. Returns 1 when `m` is a mushroom. */
static int poison_check(GameCtx *g, uint32_t rec, uint32_t m)
{
    if (!m || gm_rd32(g, m) != TAG_SHRM) return 0;
    int16_t f = (int16_t)gm_rd16(g, m + F_FRAME);
    if (g_dump_on && f < 5 && f > 0 && !gm_rd8(g, rec + R_DIVE)) fprintf(stderr, "DIVE: mushroom %06X at %d,%d frame %d, head record %06X%c", m, asr16(gm_rd32(g, m + F_POS_H)), asr16(gm_rd32(g, m + F_POS_V)), f, rec, 10);
    if (f < 5 && f > 0) {
        if (!gm_rd8(g, rec + R_DIVE)) sound(g, gm_rd32(g, A5G(g, S_POISON)), 3);
        gm_wr8(g, rec + R_DIVE, 1);
    }
    return 1;
}

static uint32_t find_overlap(GameCtx *g, uint32_t s)
{
    if (g->native_overlap) return engine_overlap(g, s, gm_rd32(g, A5G(g, G_PEDE_LIST)));
    uint8_t raw[8];
    put32(raw, s); put32(raw + 4, gm_rd32(g, A5G(g, G_PEDE_LIST)));
    return ext(g, EXT_FIND_OVERLAP, raw, 8);
}

int g_coll_log;
int g_pede_collisions;                                           /* test counter: head-on meetings of two heads (watch_pedes) */

void move_pede(GameCtx *g, uint32_t s)
{
    int32_t d6 = asr16(gm_rd32(g, s + F_POS_H));
    int32_t d5 = asr16(gm_rd32(g, s + F_POS_V));
    gm_wr32(g, cell32(g, G_PEDE_GRID, (d6 + 8) >> 4, (d5 + 8) >> 4), 0);      /* leave the occupancy grid */
    uint32_t rec = gm_rd32(g, s + F_SCRATCH);
    uint32_t tag = gm_rd32(g, s);

    if (tag == TAG_PBOD) {
        /* a body follows the oldest recorded position of the segment ahead */
        uint32_t ahead = gm_rd32(g, gm_rd32(g, rec + R_AHEAD) + F_SCRATCH);
        uint32_t i = (uint32_t)((int32_t)(int16_t)gm_rd16(g, ahead) * 12);
        d5 = (int32_t)gm_rd32(g, ahead + 8 + i);
        d6 = (int32_t)gm_rd32(g, ahead + 4 + i);
        if ((int16_t)gm_rd16(g, A5G(g, G_V_MAX)) < d5) d5 = (int16_t)gm_rd16(g, A5G(g, G_V_MAX));
        if ((int16_t)gm_rd16(g, A5G(g, G_V_MIN)) > d5) d5 = (int16_t)gm_rd16(g, A5G(g, G_V_MIN));
        if ((int16_t)gm_rd16(g, A5G(g, G_H_MAX)) < d6) d6 = (int16_t)gm_rd16(g, A5G(g, G_H_MAX));
        if ((int16_t)gm_rd16(g, A5G(g, G_H_MIN)) > d6) d6 = (int16_t)gm_rd16(g, A5G(g, G_H_MIN));
        gm_wr8(g, rec + R_DIR, gm_rd8(g, ahead + 0xC + i));
        gm_wr8(g, rec + R_NEXT, gm_rd8(g, ahead + 0xD + i));
        gm_wr16(g, rec + R_TARGET, gm_rd16(g, ahead + 0xE + i));
        gm_wr16(g, ahead, (uint16_t)(gm_rd16(g, ahead) + 1));
        if ((int16_t)gm_rd16(g, ahead) >= 10) gm_wr16(g, ahead, 0);
        ring_put(g, rec, d5, d6);
    }
    if (tag == TAG_PHED) {
        int32_t step = (int16_t)gm_rd16(g, A5G(g, G_STEP));
        if (gm_rd8(g, rec + R_DIVE)) gm_wr8(g, rec + R_DIR, 4);
        int8_t dir = (int8_t)gm_rd8(g, rec + R_DIR);
        if (dir > 0 && dir < 5) {
            switch (dir) {
            case 1: {                                   /* towards H = 0 */
                d6 -= step;
                int32_t d6_own = d6;
                uint32_t m = gm_rd32(g, cell32(g, G_GRID, d6 >> 4, d5 >> 4));
                if ((int16_t)gm_rd16(g, A5G(g, G_H_MIN)) > d6) {
                    d6 = (int16_t)gm_rd16(g, A5G(g, G_H_MIN));
                    turn(g, rec, 2, d5);
                } else if (poison_check(g, rec, m)) {
                    turn(g, rec, 2, d5);
                } else {
                    uint32_t o = find_overlap(g, s);
                    if (!o) break;
                    uint32_t orec = gm_rd32(g, o + F_SCRATCH);
                    if (o == gm_rd32(g, rec + R_BEHIND)) break;
                    if (gm_rd8(g, orec + R_DIR) != 2) break;
                    d6 = asr16(gm_rd32(g, o + F_POS_H)) + (int16_t)gm_rd16(g, o + F_W);
                    if ((int16_t)gm_rd16(g, A5G(g, G_H_MAX)) < d6) {
                        if (!g->fix_glide) d6 = (int16_t)gm_rd16(g, A5G(g, G_H_MAX));       /* the original puts it at the wall, on top of a head that is already there */
                        else d6 = d6_own;
                    }
                    g_pede_collisions++;
                    if (g_coll_log) fprintf(stderr, "COLL %u %06X %06X x %d y %d dirs 1/2 next %d/%d\n", gm_rd32(g, A5G(g, G_TICKS)), s, o, (int)d6, (int)d5, (int)gm_rd8(g, rec + R_NEXT), (int)gm_rd8(g, orec + R_NEXT));
                    turn(g, rec, 2, d5);
                    if (gm_rd8(g, orec + R_DIR) == 2) turn(g, orec, 1, d5);
                }
                break;
            }
            case 2: {                                   /* towards H = max */
                d6 += step;
                int32_t d6_own = d6;
                int32_t d4 = (0x10 + d6) >> 4;
                if (d4 >= 0x1A) d4 = 0x1A;
                uint32_t m = gm_rd32(g, cell32(g, G_GRID, d4, d5 >> 4));
                if ((int16_t)gm_rd16(g, A5G(g, G_H_MAX)) < d6) {
                    d6 = (int16_t)gm_rd16(g, A5G(g, G_H_MAX));
                    turn(g, rec, 1, d5);
                } else if (poison_check(g, rec, m)) {
                    turn(g, rec, 1, d5);
                } else {
                    uint32_t o = find_overlap(g, s);
                    if (!o) break;
                    uint32_t orec = gm_rd32(g, o + F_SCRATCH);
                    if (o == gm_rd32(g, rec + R_BEHIND)) break;
                    if (gm_rd8(g, orec + R_DIR) != 1) break;
                    d6 = asr16(gm_rd32(g, o + F_POS_H)) - (int16_t)gm_rd16(g, o + F_W);
                    if (d6 < 0) d6 = g->fix_glide ? d6_own : 0;
                    g_pede_collisions++;
                    if (g_coll_log) fprintf(stderr, "COLL %u %06X %06X x %d y %d dirs 2/1 next %d/%d\n", gm_rd32(g, A5G(g, G_TICKS)), s, o, (int)d6, (int)d5, (int)gm_rd8(g, rec + R_NEXT), (int)gm_rd8(g, orec + R_NEXT));
                    turn(g, rec, 1, d5);
                    if (gm_rd8(g, orec + R_DIR) == 1) turn(g, orec, 2, d5);
                }
                break;
            }
            case 3:                                     /* up */
                d5 -= step;
                if ((int16_t)gm_rd16(g, A5G(g, G_TOP_LIMIT)) > d5) {
                    d5 = (int16_t)gm_rd16(g, A5G(g, G_TOP_LIMIT));
                    gm_wr8(g, rec + R_DIR, gm_rd8(g, rec + R_NEXT));
                    gm_wr8(g, rec + R_NEXT, 4);
                } else {
                    int16_t t = (int16_t)gm_rd16(g, rec + R_TARGET);
                    if (t > d5) {
                        d5 = t;
                        gm_wr8(g, rec + R_DIR, gm_rd8(g, rec + R_NEXT));
                        gm_wr8(g, rec + R_NEXT, 3);
                    }
                }
                break;
            case 4: {                                   /* down */
                d5 += step;
                int32_t d3 = d5 >> 4;
                uint32_t m = gm_rd32(g, cell32(g, G_GRID, d6 >> 4, d3));
                poison_check(g, rec, m);
                if (gm_rd8(g, rec + R_DIVE)) {
                    gm_wr16(g, rec + R_TARGET, (uint16_t)d3);
                    gm_wr8(g, rec + R_NEXT, 2);
                }
                if ((int16_t)gm_rd16(g, A5G(g, G_V_MAX)) < d5) {
                    d5 = (int16_t)gm_rd16(g, A5G(g, G_V_MAX));
                    gm_wr8(g, rec + R_DIR, gm_rd8(g, rec + R_NEXT));
                    if (gm_rd8(g, rec + R_DIVE)) {
                        gm_wr8(g, rec + R_DIR, 3);
                        gm_wr8(g, rec + R_NEXT, 2);
                        gm_wr16(g, rec + R_TARGET, (uint16_t)(-0x10 + (int16_t)d5));
                    } else {
                        gm_wr8(g, rec + R_NEXT, 3);
                    }
                    if (!gm_rd8(g, A5G(g, G_TOUCHED)) && !gm_rd8(g, rec + R_DIVE)) {
                        gm_wr8(g, A5G(g, G_TOUCHED), 1);
                        gm_wr32(g, A5G(g, G_TOUCH_TICK), gm_rd32(g, A5G(g, G_TICKS)));
                    }
                    gm_wr8(g, rec + R_DIVE, 0);
                } else {
                    int16_t t = (int16_t)gm_rd16(g, rec + R_TARGET);
                    if (t < d5 && !gm_rd8(g, rec + R_DIVE)) {
                        d5 = t;
                        gm_wr8(g, rec + R_DIR, gm_rd8(g, rec + R_NEXT));
                        gm_wr8(g, rec + R_NEXT, 4);
                    }
                }
                break;
            }
            }
        }
        ring_put(g, rec, d5, d6);
    }
    gm_wr32(g, cell32(g, G_PEDE_GRID, (d6 + 8) >> 4, (d5 + 8) >> 4), s);      /* enter the occupancy grid */
    gm_wr32(g, s + F_POS_H, shl16(d6));
    gm_wr32(g, s + F_POS_V, shl16(d5));
}

/* ---- per-step update (seg3_1BB6) and the score / extra-life / HUD update it calls (seg3_1A72) ---- */
enum {
    G_DISPLAY_FLAG = -0x715, G_DISPLAY_TICK = -0x70C, G_PALETTE_H = -0x4C4, G_PALETTE_H2 = -0x71C, G_HOOK_ARG = -0x72E,
    G_TOUCH_FLAG = -0x6D2, G_TIMER_A = -0x686, G_TIMER_B = -0x6AE, G_TIMER_C = -0x6B2,
    S_TIMER_A = -0x538, S_TIMER_B = -0x534, S_TIMER_C = -0x518, G_MUSH_LIMIT = -0x6A2, G_FLEA_LIMIT = -0x6BC,
    G_LEVEL_SHOWN = -0x734, G_LEVEL_SHOWN2 = -0x732, G_DIGITS = -0x6A0, G_DEAD = -0x6F6, G_PLAYER = -0x59C, G_LIST_BAD = -0x640,
    G_SHIELD = -0x66A, G_MUSIC = -0x708, G_DEATH_TICK = -0x6F4, S_DEATH = -0x56C, S_POWER = -0x560, S_LIFE = -0x568,
    G_P_66D = -0x66D, G_P_66C = -0x66C, G_P_66B = -0x66B, G_P_669 = -0x669, G_P_66E = -0x66E, G_SPEED = -0x6C4, G_SPEED2 = -0x6CA,
    G_LIVES = -0x6FA, G_LIVES_SHOWN = -0x6F8, G_RESET = -0x730, G_SCORE_SHOWN = -0x6FE, G_SCORE_TICK = -0x696, G_SCORE_DIGITS = -0x69C,
    G_LIFE_MARK = -0x68E, G_NOBONUS = -0x6A8, G_BONUS_TICK = -0x67C, G_BONUS = -0x682, G_BONUS_SHOWN = -0x67E, G_BONUS_SHOWN_SRC = -0x680,
    G_BONUS_DIGITS = -0x678
};

#define TRAP_SETENTRIES 0xAA3Fu

static int32_t ldiv32(int32_t a, int32_t b) { return a / b; }   /* the library signed long divide (truncating) */

static void call0(GameCtx *g, uint32_t addr) { ext(g, (int)addr, 0, 0); }
static void call_sound(GameCtx *g, int off, uint16_t pri) { sound(g, gm_rd32(g, A5G(g, off)), pri); }

static void show_number(GameCtx *g, int32_t n, uint32_t digits, uint8_t flag)
{
    uint8_t raw[9];
    put32(raw, (uint32_t)n); put32(raw + 4, digits); raw[8] = flag;
    ext(g, 0x415E4, raw, 9);
}

/* seg3_1A72: lives, score counting up, bonus counting down */
void update_score_display(GameCtx *g)
{
    if (gm_rd8(g, A5G(g, G_P_66E))) {
        call0(g, 0x41428);
        gm_wr8(g, A5G(g, G_P_66E), 0);
    }
    uint32_t score = gm_rd32(g, A5G(g, G_SCORE));
    if (score != gm_rd32(g, A5G(g, G_SCORE_SHOWN)) && gm_rd32(g, A5G(g, G_SCORE_TICK)) != gm_rd32(g, A5G(g, G_TICKS))) {
        int16_t gap = (int16_t)(gm_rd16(g, A5G(g, G_SCORE) + 2) - gm_rd16(g, A5G(g, G_SCORE_SHOWN) + 2));
        uint32_t shown = gm_rd32(g, A5G(g, G_SCORE_SHOWN));
        if (gap < 0x32) shown += 1;
        else if (gap < 0xFA) shown += 5;
        else if (gap < 0x1F4) shown += 10;
        else if (gap < 0x9C4) shown += 0x32;
        else shown += 0x3E8;
        gm_wr32(g, A5G(g, G_SCORE_SHOWN), shown);
        int32_t d7 = (int32_t)shown;
        while (d7 > 0x98967F) d7 -= 0x98967F;
        show_number(g, d7, gm_rd32(g, A5G(g, G_SCORE_DIGITS)), 0);
        gm_wr32(g, A5G(g, G_SCORE_TICK), gm_rd32(g, A5G(g, G_TICKS)));
        if ((int32_t)(score - gm_rd32(g, A5G(g, G_LIFE_MARK))) >= 0x4E20) {
            gm_wr32(g, A5G(g, G_LIFE_MARK), (uint32_t)lmul(ldiv32((int32_t)score, 0x4E20), 0x4E20));
            if ((int16_t)gm_rd16(g, A5G(g, G_LIVES)) < 8) {
                gm_wr16(g, A5G(g, G_LIVES), (uint16_t)(gm_rd16(g, A5G(g, G_LIVES)) + 1));
                call_sound(g, S_LIFE, 3);
            }
        }
    }
    if (gm_rd16(g, A5G(g, G_LIVES)) != gm_rd16(g, A5G(g, G_LIVES_SHOWN))) {
        call0(g, 0x4157A);
        gm_wr16(g, A5G(g, G_LIVES_SHOWN), gm_rd16(g, A5G(g, G_LIVES)));
    }
    if (!gm_rd8(g, A5G(g, G_NOBONUS)) && (int32_t)gm_rd32(g, A5G(g, G_TICKS)) > (int32_t)gm_rd32(g, A5G(g, G_BONUS_TICK))) {
        gm_wr32(g, A5G(g, G_BONUS_TICK), gm_rd32(g, A5G(g, G_TICKS)) + 0x14);
        gm_wr32(g, A5G(g, G_BONUS), gm_rd32(g, A5G(g, G_BONUS)) - 10);
        if ((int32_t)gm_rd32(g, A5G(g, G_BONUS)) < 0) gm_wr32(g, A5G(g, G_BONUS), 0);
    }
    if ((int32_t)(int16_t)gm_rd16(g, A5G(g, G_BONUS_SHOWN)) != (int32_t)gm_rd32(g, A5G(g, G_BONUS))) {
        show_number(g, (int32_t)gm_rd32(g, A5G(g, G_BONUS)), gm_rd32(g, A5G(g, G_BONUS_DIGITS)), 1);
        gm_wr16(g, A5G(g, G_BONUS_SHOWN), gm_rd16(g, A5G(g, G_BONUS_SHOWN_SRC)));
    }
}

static void set_entries_trap(GameCtx *g)
{
    uint32_t p = gm_rd32(g, gm_rd32(g, A5G(g, G_PALETTE_H)));
    uint8_t raw[8];
    (void)p;
    put32(raw, p + 8); put16(raw + 4, gm_rd16(g, p + 6)); put16(raw + 6, 0);
    ext(g, (int)TRAP_SETENTRIES, raw, 8);
}

static void player_dies(GameCtx *g)
{
    uint32_t p = gm_rd32(g, gm_rd32(g, A5G(g, G_PALETTE_H2)));
    uint8_t raw[8];
    put32(raw, p + 8); put16(raw + 4, gm_rd16(g, p + 6)); put16(raw + 6, 0);
    ext(g, (int)TRAP_SETENTRIES, raw, 8);
    gm_wr8(g, A5G(g, G_DISPLAY_FLAG), 1);
    gm_wr32(g, A5G(g, G_DISPLAY_TICK), gm_rd32(g, A5G(g, G_TICKS)));
    if (gm_rd8(g, A5G(g, G_MUSIC))) {
        uint8_t w[2]; put16(w, 4);
        ext(g, (int)(g->a5 + 0x3DA), w, 2);
    }
    call_sound(g, S_DEATH, 3);
    gm_wr8(g, A5G(g, G_DEAD), 1);
    gm_wr32(g, A5G(g, G_DEATH_TICK), gm_rd32(g, A5G(g, G_TICKS)));
    uint32_t pl = gm_rd32(g, A5G(g, G_PLAYER));
    uint8_t e[8];
    put32(e, pl); put32(e + 4, gm_rd32(g, A5G(g, G_EXPL_VALUE)));
    ext(g, EXT_EXPLOSION, e, 8);
    uint8_t one[4]; put32(one, pl);
    ext(g, 0x4363A, one, 4);
    sprite_deactivate(g, pl);
}

static uint32_t find_in_list(GameCtx *g, uint32_t sprite, uint32_t list)
{
    if (g->native_overlap) return engine_overlap(g, sprite, list);
    uint8_t raw[8];
    put32(raw, sprite); put32(raw + 4, list);
    return ext(g, EXT_FIND_OVERLAP, raw, 8);
}

/* returns D0 as the original does (the lives shown, low word) */
uint16_t step_update(GameCtx *g)
{
    uint32_t ticks = gm_rd32(g, A5G(g, G_TICKS)) + 1;
    gm_wr32(g, A5G(g, G_TICKS), ticks);
    if (gm_rd8(g, A5G(g, G_DISPLAY_FLAG)) && 0x14 < (int32_t)(ticks - gm_rd32(g, A5G(g, G_DISPLAY_TICK)))) {
        set_entries_trap(g);
        gm_wr8(g, A5G(g, G_DISPLAY_FLAG), 0);
    }
    {
        uint8_t raw[4]; put32(raw, gm_rd32(g, A5G(g, G_HOOK_ARG)));
        ext(g, (int)(g->a5 + 0x4E2), raw, 4);
    }
    if (gm_rd8(g, A5G(g, G_TOUCH_FLAG))) call0(g, 0x42128);
    static const struct { int when, snd; uint32_t fn; } timers[3] = {
        { G_TIMER_A, S_TIMER_A, 0x43A36 }, { G_TIMER_B, S_TIMER_B, 0x43B02 }, { G_TIMER_C, S_TIMER_C, 0x43BD8 } };
    for (int i = 0; i < 3; i++) {
        uint32_t t = gm_rd32(g, A5G(g, timers[i].when));
        if (t && (int32_t)ticks > (int32_t)t) {
            gm_wr32(g, A5G(g, timers[i].when), 0);
            call_sound(g, timers[i].snd, 3);
            call0(g, timers[i].fn);
        }
    }
    int32_t score = (int32_t)gm_rd32(g, A5G(g, G_SCORE));
    uint16_t limit;
    if (score < 0x13880) limit = 0x12;
    else if (score < 0x186A0) limit = 0x13;
    else if (score < 0x1D4C0) limit = 0x14;
    else if (score < 0x222E0) limit = 0x15;
    else if (score < 0x27100) limit = 0x16;
    else if (score < 0x2BF20) limit = 0x17;
    else limit = 0x12;
    gm_wr16(g, A5G(g, G_MUSH_LIMIT), limit);
    if (score < 0x4E20) gm_wr16(g, A5G(g, G_FLEA_LIMIT), 5);
    else if (score < 0x1D4C0) gm_wr16(g, A5G(g, G_FLEA_LIMIT), 9);
    else if (score < 0x222E0) gm_wr16(g, A5G(g, G_FLEA_LIMIT), 0xF);
    else gm_wr16(g, A5G(g, G_FLEA_LIMIT), (uint16_t)(ldiv32(score - 0x222E0, 0x7530) + 0x10));
    if (gm_rd16(g, A5G(g, G_LEVEL_SHOWN)) != gm_rd16(g, A5G(g, G_LEVEL_SHOWN2))) {
        gm_wr16(g, A5G(g, G_LEVEL_SHOWN2), (uint16_t)(gm_rd16(g, A5G(g, G_LEVEL_SHOWN2)) + 1));
        int16_t d7 = (int16_t)gm_rd16(g, A5G(g, G_LEVEL_SHOWN2));
        while (d7 > 0x63) d7 = (int16_t)(d7 - 0x63);
        show_number(g, d7, gm_rd32(g, A5G(g, G_DIGITS)), d7 < 10 ? 1 : 0);
    }
    update_score_display(g);
    if (!gm_rd8(g, A5G(g, G_DEAD))) call0(g, 0x42256);
    call0(g, 0x42334);
    if ((uint8_t)ext(g, 0x43348, 0, 0)) { if (g->native_endwave) game_pause(g); else call0(g, 0x42596); }
    if (!gm_rd8(g, A5G(g, G_DEAD))) {
        uint32_t pl = gm_rd32(g, A5G(g, G_PLAYER));
        uint32_t a4 = find_in_list(g, pl, gm_rd32(g, A5G(g, G_PEDE_LIST)));
        if (a4 && !gm_rd8(g, A5G(g, G_SHIELD))) player_dies(g);
        a4 = find_in_list(g, gm_rd32(g, A5G(g, G_PLAYER)), gm_rd32(g, A5G(g, G_LIST_BAD)));
        if (a4) {
            uint32_t tag = gm_rd32(g, a4);
            if (tag == 0x79756D79u) {                       /* 'yumy': a coin */
                sprite_deactivate(g, a4);
                gm_wr8(g, a4 + F_REMOVE, 1);
                uint16_t r = rnd(g, 7);
                if (r != 2) call_sound(g, S_POWER, 3);
                switch (r) {
                case 0: gm_wr8(g, A5G(g, G_P_66D), 1); break;
                case 1: gm_wr8(g, A5G(g, G_P_66C), 1); break;
                case 2:
                    if ((int16_t)gm_rd16(g, A5G(g, G_LIVES)) < 8) {
                        gm_wr16(g, A5G(g, G_LIVES), (uint16_t)(gm_rd16(g, A5G(g, G_LIVES)) + 1));
                        call_sound(g, S_LIFE, 3);
                    } else {
                        call_sound(g, S_LEAVE, 3);
                    }
                    break;
                case 3:
                    gm_wr8(g, A5G(g, G_P_66B), 1);
                    gm_wr32(g, A5G(g, G_SPEED), 0xE);
                    gm_wr16(g, A5G(g, G_SPEED2), 10);
                    break;
                case 4: call0(g, 0x42048); break;
                case 5: call0(g, 0x420CC); break;
                case 6: gm_wr8(g, A5G(g, G_P_669), 1); break;
                default: break;
                }
                gm_wr8(g, A5G(g, G_P_66E), 1);
            } else if (tag != 0x70737963u) {                 /* not 'psyc' either: a hazard */
                if (!gm_rd8(g, A5G(g, G_SHIELD))) player_dies(g);
            }
        }
    }
    if (gm_rd8(g, A5G(g, G_DEAD)) && 0xB4 < (int32_t)(gm_rd32(g, A5G(g, G_TICKS)) - gm_rd32(g, A5G(g, G_DEATH_TICK)))) {
        if (!gm_rd8(g, A5G(g, G_P_669))) {
            gm_wr8(g, A5G(g, G_P_66D), 0); gm_wr8(g, A5G(g, G_P_66C), 0); gm_wr8(g, A5G(g, G_P_66B), 0); gm_wr8(g, A5G(g, G_SHIELD), 0);
            gm_wr32(g, A5G(g, G_SPEED), 0);
            gm_wr16(g, A5G(g, G_SPEED2), 1);
        } else {
            call_sound(g, S_POWER, 3);
        }
        gm_wr8(g, A5G(g, G_P_669), 0);
        gm_wr8(g, A5G(g, G_P_66E), 1);
        gm_wr8(g, A5G(g, G_DEAD), 0);
        if (g->native_endwave) end_round_repair(g, 0x91);
        else { uint8_t w[2]; put16(w, 0x91); ext(g, 0x426C2, w, 2); }
        gm_wr16(g, A5G(g, G_LIVES), (uint16_t)(gm_rd16(g, A5G(g, G_LIVES)) - 1));
        { uint8_t l[4]; put32(l, gm_rd32(g, A5G(g, G_PLAYER))); ext(g, 0x40902, l, 4); }
        call0(g, 0x4299A); call0(g, 0x400EA); call0(g, 0x40098); call0(g, 0x405CE); call0(g, 0x400C2);
    }
    if ((int16_t)gm_rd16(g, A5G(g, G_LIVES)) < 0 && !gm_rd8(g, A5G(g, G_DEAD))) gm_wr8(g, A5G(g, G_RESET), 0);
    return gm_rd16(g, A5G(g, G_LIVES_SHOWN));
}

/* ---- sprite engine (CODE 3, "Animation Tool"): lists, sprites, the move pass and the erase/draw pass ----
 *
 * The engine keeps its state in a record that A5-0x758 points at (E below); lists and sprites are linked records. Drawing is done by small
 * pieces of code generated at run time (one per sprite frame); the port reports each such call as a blit (address, A0, A1, D0, D1) so a test
 * can compare the order and arguments, and the real drawing is done by the SDL layer.
 */
enum {
    G_ENGINE = -0x758, G_ENGINE_READY = -0x75A,
    E_FIRST_LIST = 4, E_LAST_LIST = 8, E_BOUNDS = 0x14, E_ROWS_SCREEN = 0x18, E_ROWS_WORK = 0x1C, E_ROWS_SAVED = 0x20,
    E_W24 = 0x24, E_W26 = 0x26, E_W28 = 0x28, E_INTERVAL = 0x2A, E_LAST_DRAW = 0x2C, E_MOVED = 0x30, E_DRAWN = 0x31,
    E_OFFSET_H = 0x3A, E_OFFSET_V = 0x3C,
    L_TAG = 0, L_FIRST = 4, L_LAST = 8, L_ACTIVE = 0xC, L_PREV = 0xE, L_NEXT = 0x12, L_SIZE = 0x16,
    S_TAG = 0, S_CLASS = 4, S_RECT = 8, S_OLD_RECT = 0x10, S_MOVE_CB = 0x20, S_LAST_MOVE = 0x24, S_MOVE_INTERVAL = 0x28,
    S_OLD_H = 0x32, S_OLD_V = 0x36, S_FRAME_CB = 0x42, S_FRAME_OLD = 0x4A, S_LAST_FRAME = 0x4C, S_FRAME_INTERVAL = 0x50,
    S_PREV = 0x56, S_NEXT = 0x5A, S_SIZE = 0x5E
};

uint32_t gm_alloc(GameCtx *g, uint32_t size)      /* _NewPtr: a bump allocator, the same as the test harness uses */
{
    if (g->reuse_memory) {
        for (int i = 0; i < 8; i++)
            if (g->free_size[i] == size && g->free_head[i]) {
                uint32_t q = g->free_head[i];
                g->free_head[i] = gm_rd32(g, q);
                return q;
            }
        uint32_t q = g->heap_next + 4;
        gm_wr32(g, g->heap_next, size);
        g->heap_next += 4 + ((size + 3) & ~3u);
        return q;
    }
    uint32_t p = g->heap_next;
    g->heap_next += (size + 3) & ~3u;
    return p;
}

static void dispose_ptr(GameCtx *g, uint32_t p)
{
    uint8_t raw[4]; put32(raw, p);
    ext(g, 0xA01F, raw, 4);
    if (g->reuse_memory && p) {
        uint32_t size = gm_rd32(g, p - 4);
        for (int i = 0; i < 8; i++)
            if (g->free_size[i] == size || g->free_size[i] == 0) {
                g->free_size[i] = size;
                gm_wr32(g, p, g->free_head[i]);
                g->free_head[i] = p;
                return;
            }
    }
}

uint32_t engine_list_create(GameCtx *g, uint32_t tag)                      /* seg3_0712 */
{
    if (!gm_rd8(g, A5G(g, G_ENGINE_READY))) return 0;
    uint32_t l = gm_alloc(g, L_SIZE);
    uint32_t e = gm_rd32(g, A5G(g, G_ENGINE));
    gm_wr32(g, l + L_TAG, tag);
    gm_wr32(g, l + L_FIRST, 0);
    gm_wr32(g, l + L_LAST, 0);
    gm_wr8(g, l + L_ACTIVE, 1);
    gm_wr32(g, l + L_PREV, gm_rd32(g, e + E_LAST_LIST));
    gm_wr32(g, l + L_NEXT, 0);
    if (gm_rd32(g, e + E_LAST_LIST)) gm_wr32(g, gm_rd32(g, e + E_LAST_LIST) + L_NEXT, l);
    if (!gm_rd32(g, e + E_FIRST_LIST)) gm_wr32(g, e + E_FIRST_LIST, l);
    gm_wr32(g, e + E_LAST_LIST, l);
    return l;
}

uint32_t engine_sprite_alloc(GameCtx *g, uint32_t cls, uint32_t tag, uint32_t list)   /* seg3_0782 */
{
    if (!gm_rd8(g, A5G(g, G_ENGINE_READY))) return 0;
    uint32_t s = gm_alloc(g, S_SIZE);
    uint32_t e = gm_rd32(g, A5G(g, G_ENGINE));
    gm_wr32(g, s + S_TAG, tag);
    gm_wr32(g, s + S_CLASS, cls);
    uint32_t data = gm_rd32(g, cls);
    gm_wr32(g, s + 8, gm_rd32(g, data + 6));
    gm_wr32(g, s + 0xC, gm_rd32(g, data + 0xA));
    gm_wr32(g, s + 0x10, gm_rd32(g, s + 8));
    gm_wr32(g, s + 0x14, gm_rd32(g, s + 0xC));
    gm_wr16(g, s + F_W, (uint16_t)(gm_rd16(g, s + 0xE) - gm_rd16(g, s + 0xA)));
    gm_wr16(g, s + F_H, (uint16_t)(gm_rd16(g, s + 0xC) - gm_rd16(g, s + 8)));
    gm_wr8(g, s + F_VISIBLE, 1);
    gm_wr8(g, s + F_DIRTY, 1);
    gm_wr8(g, s + F_HIDDEN, 0);
    gm_wr8(g, s + F_REMOVE, 0);
    gm_wr32(g, s + S_MOVE_CB, g->a5 + 0x242);          /* jump-table slots of the default move and frame routines */
    gm_wr32(g, s + 0x24, 0);
    gm_wr16(g, s + S_MOVE_INTERVAL, gm_rd16(g, e + E_INTERVAL));
    gm_wr32(g, s + F_POS_H, 0); gm_wr32(g, s + F_POS_V, 0);
    gm_wr32(g, s + S_OLD_H, 0); gm_wr32(g, s + S_OLD_V, 0);
    gm_wr32(g, s + F_VEL_H, 0); gm_wr32(g, s + F_VEL_V, 0);
    gm_wr32(g, s + S_FRAME_CB, g->a5 + 0x24A);
    gm_wr16(g, s + F_FRAMES, gm_rd16(g, data + 4));
    gm_wr16(g, s + F_FRAME, 1);
    gm_wr16(g, s + S_FRAME_OLD, 1);
    gm_wr32(g, s + S_LAST_FRAME, 0);
    gm_wr16(g, s + S_FRAME_INTERVAL, gm_rd16(g, e + E_INTERVAL));
    gm_wr32(g, s + F_SCRATCH, 0);
    gm_wr32(g, s + S_PREV, gm_rd32(g, list + L_LAST));
    gm_wr32(g, s + S_NEXT, 0);
    if (gm_rd32(g, list + L_LAST)) gm_wr32(g, gm_rd32(g, list + L_LAST) + S_NEXT, s);
    if (!gm_rd32(g, list + L_FIRST)) gm_wr32(g, list + L_FIRST, s);
    gm_wr32(g, list + L_LAST, s);
    return s;
}

void engine_unlink(GameCtx *g, uint32_t list, uint32_t s)                   /* seg3_0884 */
{
    if (!gm_rd8(g, A5G(g, G_ENGINE_READY))) return;
    uint32_t prev = gm_rd32(g, s + S_PREV), next = gm_rd32(g, s + S_NEXT);
    if (prev) gm_wr32(g, prev + S_NEXT, next);
    if (next) gm_wr32(g, next + S_PREV, prev);
    if (gm_rd32(g, list + L_FIRST) == s) gm_wr32(g, list + L_FIRST, next);
    if (gm_rd32(g, list + L_LAST) == s) gm_wr32(g, list + L_LAST, prev);
    dispose_ptr(g, s);
}

void engine_activate(GameCtx *g, uint32_t s)                                /* seg3_0902 */
{
    if (gm_rd8(g, s + F_VISIBLE)) return;
    gm_wr8(g, s + F_VISIBLE, 1);
    gm_wr8(g, s + F_DIRTY, 0);
    gm_wr32(g, s + S_OLD_H, gm_rd32(g, s + F_POS_H));
    gm_wr32(g, s + S_OLD_V, gm_rd32(g, s + F_POS_V));
    int32_t h = asr16(gm_rd32(g, s + F_POS_H)), v = asr16(gm_rd32(g, s + F_POS_V));
    gm_wr16(g, s + 0xA, (uint16_t)h);
    gm_wr16(g, s + 0x8, (uint16_t)v);
    gm_wr16(g, s + 0xE, (uint16_t)(gm_rd16(g, s + F_W) + h));
    gm_wr16(g, s + 0xC, (uint16_t)(gm_rd16(g, s + F_H) + v));
    gm_wr16(g, s + 0x12, (uint16_t)h);
    gm_wr16(g, s + 0x10, (uint16_t)v);
    gm_wr16(g, s + 0x16, (uint16_t)(gm_rd16(g, s + F_W) + h));
    gm_wr16(g, s + 0x14, (uint16_t)(gm_rd16(g, s + F_H) + v));
}

uint32_t engine_overlap(GameCtx *g, uint32_t s, uint32_t list)              /* seg3_055E: first visible sprite of `list` overlapping `s` */
{
    for (uint32_t o = gm_rd32(g, list + L_FIRST); o; o = gm_rd32(g, o + S_NEXT)) {
        if (!gm_rd8(g, o + F_VISIBLE) || o == s) continue;
        int16_t top = (int16_t)gm_rd16(g, s + 8), left = (int16_t)gm_rd16(g, s + 0xA);
        int16_t bottom = (int16_t)gm_rd16(g, s + 0xC), right = (int16_t)gm_rd16(g, s + 0xE);
        int16_t otop = (int16_t)gm_rd16(g, o + 8), oleft = (int16_t)gm_rd16(g, o + 0xA);
        int16_t obottom = (int16_t)gm_rd16(g, o + 0xC), oright = (int16_t)gm_rd16(g, o + 0xE);
        if (left >= oright) continue;
        if (top >= obottom) continue;
        if (right <= oleft) continue;
        if (bottom <= otop) continue;
        return o;
    }
    return 0;
}

/* callbacks are 68k addresses (or jump-table slots); the ported ones are called directly, anything else is reported */
static void run_callback(GameCtx *g, uint32_t addr, uint32_t s)
{
    uint8_t raw[4]; put32(raw, s);
    switch (addr) {
    case 0x40B0E: sprite_move_default(g, s); return;
    case 0x44286: move_gecko(g, s); return;
    case 0x44330: move_scorpion(g, s); return;
    case 0x440B4: move_flea(g, s); return;
    case 0x44954: move_spaceship(g, s); return;
    case 0x44AE6: move_coin(g, s); return;
    case 0x44832: case 0x447E8: move_timed_bonus(g, s); return;
    case 0x44A9A: move_bullet(g, s); return;
    case 0x44BC6: move_pede(g, s); return;
    case 0x40BD8: case 0x44260: case 0x4492E: case 0x45554: frame_next_to_count(g, s); return;
    case 0x44310: frame_gecko(g, s); return;
    case 0x44BA6: frame_scorpion(g, s); return;
    case 0x447C2: frame_psychedelic(g, s); return;
    case 0x451A4: frame_pede_body(g, s); return;
    case 0x440A4: case 0x440AC: case 0x44A92: case 0x44ADE: case 0x4482A: case 0x44B9E: case 0x44726: return;   /* routines that only return */
    case 0x45216: move_shooter(g, s); return;
    case 0x454C8: frame_shooter(g, s); return;
    case 0x454DA: move_shot(g, s); return;
    case 0x4454C: move_yummy(g, s); return;
    case 0x44674: frame_next_to_count(g, s); return;
    case 0x4490E: move_shield(g, s); return;
    case 0x44874: frame_shield(g, s); return;
    case 0x44B6A: frame_explosion(g, s); return;
    case 0x4472E: move_score_digit(g, s); return;
    case 0x444EE: move_fragment(g, s); return;
    case 0x444BA: frame_fragment(g, s); return;
    case 0x446C8: move_spark(g, s); return;
    case 0x4469A: frame_spark(g, s); return;
    default: ext(g, (int)addr, raw, 4); return;
    }
}

static uint32_t resolve_callback(GameCtx *g, uint32_t addr)
{
    /* callbacks are often jump-table slots: the landing address holds a JMP.L to the routine */
    if (gm_rd16(g, addr) == 0x4EF9) return gm_rd32(g, addr + 2);
    return addr;
}

void engine_move_pass(GameCtx *g)                                           /* seg3_00EA */
{
    if (!gm_rd8(g, A5G(g, G_ENGINE_READY))) return;
    uint32_t e = gm_rd32(g, A5G(g, G_ENGINE));
    if (gm_rd8(g, e + E_MOVED)) return;
    uint32_t now = gm_rd32(g, 0x16A);
    for (uint32_t l = gm_rd32(g, e + E_FIRST_LIST); l; l = gm_rd32(g, l + L_NEXT)) {
        if (!gm_rd8(g, l + L_ACTIVE)) continue;
        for (uint32_t s = gm_rd32(g, l + L_FIRST); s; s = gm_rd32(g, s + S_NEXT)) {
            if (!gm_rd8(g, s + F_VISIBLE)) continue;
            uint32_t interval = gm_rd16(g, s + S_MOVE_INTERVAL);
            if (interval != 0xFFFF && (int32_t)(now - gm_rd32(g, s + S_LAST_MOVE)) >= (int32_t)interval) {
                gm_wr32(g, s + S_LAST_MOVE, now);
                uint32_t d6 = gm_rd32(g, s + F_POS_H);
                gm_wr32(g, s + S_OLD_H, d6);
                uint32_t d7 = gm_rd32(g, s + F_POS_V);
                gm_wr32(g, s + S_OLD_V, d7);
                gm_wr32(g, s + 0x10, gm_rd32(g, s + 8));
                gm_wr32(g, s + 0x14, gm_rd32(g, s + 0xC));
                run_callback(g, resolve_callback(g, gm_rd32(g, s + S_MOVE_CB)), s);
                uint32_t d4 = gm_rd32(g, s + F_POS_V);
                d7 = (d7 ^ d4) >> 16 | (d7 ^ d4) << 16;
                uint32_t d3 = gm_rd32(g, s + F_POS_H);
                d6 = (d6 ^ d3) >> 16 | (d6 ^ d3) << 16;
                if (gm_rd8(g, s + F_DIRTY) || ((d6 | d7) & 0xFFFF)) {
                    gm_wr16(g, s + 8, (uint16_t)(d4 >> 16));
                    gm_wr16(g, s + 0xA, (uint16_t)(d3 >> 16));
                    gm_wr16(g, s + 0xC, (uint16_t)((d4 >> 16) + gm_rd16(g, s + F_H)));
                    gm_wr16(g, s + 0xE, (uint16_t)((d3 >> 16) + gm_rd16(g, s + F_W)));
                    gm_wr8(g, s + F_DIRTY, 0xFF);
                }
            }
            interval = gm_rd16(g, s + S_FRAME_INTERVAL);
            if (interval != 0xFFFF && (int32_t)(now - gm_rd32(g, s + S_LAST_FRAME)) >= (int32_t)interval) {
                gm_wr32(g, s + S_LAST_FRAME, now);
                uint16_t before = gm_rd16(g, s + F_FRAME);
                gm_wr16(g, s + S_FRAME_OLD, before);
                run_callback(g, resolve_callback(g, gm_rd32(g, s + S_FRAME_CB)), s);
                if (gm_rd16(g, s + F_FRAME) != before) gm_wr8(g, s + F_DIRTY, 0xFF);
            }
        }
    }
    gm_wr8(g, e + E_MOVED, 0xFF);
    gm_wr8(g, e + E_DRAWN, 0);
}

/* address of the generated routine for a frame: class data + table[frame - 1] */
static uint32_t frame_code(GameCtx *g, uint32_t s, int frame_field, int table_field)
{
    uint32_t data = gm_rd32(g, gm_rd32(g, s + S_CLASS));
    int16_t idx = (int16_t)(gm_rd16(g, s + frame_field) - 1);
    uint32_t table = data + gm_rd32(g, data + table_field);
    return data + gm_rd32(g, table + (uint32_t)((int32_t)idx * 4));
}

static uint32_t row_ptr(GameCtx *g, uint32_t rows, int16_t top, int16_t left)
{
    return gm_rd32(g, rows + (uint32_t)((int32_t)top * 4)) + (uint32_t)(int32_t)left;
}

static void blit4(GameCtx *g, uint32_t code, uint32_t a0, uint32_t a1, uint32_t d0, uint32_t d1)
{
    uint8_t raw[16];
    put32(raw, a0); put32(raw + 4, a1); put32(raw + 8, d0); put32(raw + 12, d1);
    ext(g, (int)code, raw, 16);
}

static void blit2(GameCtx *g, uint32_t code, uint32_t a0, uint32_t d0)
{
    uint8_t raw[8];
    put32(raw, a0); put32(raw + 4, d0);
    ext(g, (int)code, raw, 8);
}

static int needs_erase(GameCtx *g, uint32_t s)    /* (visible & dirty) | (hidden & ~visible), as the original computes it on bytes */
{
    uint8_t vis = gm_rd8(g, s + F_VISIBLE), dirty = gm_rd8(g, s + F_DIRTY), hid = gm_rd8(g, s + F_HIDDEN);
    return (uint8_t)((vis & dirty) | (hid & (uint8_t)~vis)) != 0;
}

/* true if another active list holds a changed sprite whose old or new area overlaps `s` */
static int touched_by_other(GameCtx *g, uint32_t e, uint32_t s)
{
    for (uint32_t l = gm_rd32(g, e + E_FIRST_LIST); l; l = gm_rd32(g, l + L_NEXT)) {
        if (!gm_rd8(g, l + L_ACTIVE)) continue;
        for (uint32_t o = gm_rd32(g, l + L_FIRST); o; o = gm_rd32(g, o + S_NEXT)) {
            if (o == s || !needs_erase(g, o)) continue;
            int16_t ntop = (int16_t)gm_rd16(g, o + 8), nleft = (int16_t)gm_rd16(g, o + 0xA);
            int16_t otop = (int16_t)gm_rd16(g, o + 0x10), oleft = (int16_t)gm_rd16(g, o + 0x12);
            int16_t nbot = (int16_t)gm_rd16(g, o + 0xC), nright = (int16_t)gm_rd16(g, o + 0xE);
            int16_t obot = (int16_t)gm_rd16(g, o + 0x14), oright = (int16_t)gm_rd16(g, o + 0x16);
            int16_t left = nleft > oleft ? oleft : nleft;      /* union of the old and the new area */
            int16_t top = ntop > otop ? otop : ntop;
            int16_t right = nright < oright ? oright : nright;
            int16_t bottom = nbot < obot ? obot : nbot;
            int16_t sleft = (int16_t)gm_rd16(g, s + 0xA), stop = (int16_t)gm_rd16(g, s + 8);
            int16_t sright = (int16_t)gm_rd16(g, s + 0xE), sbottom = (int16_t)gm_rd16(g, s + 0xC);
            if ((int16_t)(sleft - right) >= 0) continue;
            if ((int16_t)(stop - bottom) >= 0) continue;
            if ((int16_t)(sright - left) < 0) continue;
            if ((int16_t)(sbottom - top) < 0) continue;
            return 1;
        }
    }
    return 0;
}

void engine_draw_pass(GameCtx *g)                                           /* seg3_020C and the code that follows it */
{
    if (!gm_rd8(g, A5G(g, G_ENGINE_READY))) return;
    uint32_t e = gm_rd32(g, A5G(g, G_ENGINE));
    if (gm_rd8(g, e + E_DRAWN)) return;
    if ((int32_t)(int16_t)gm_rd16(g, e + E_INTERVAL) > (int32_t)(gm_rd32(g, 0x16A) - gm_rd32(g, e + E_LAST_DRAW))) return;
    uint32_t saved_rows = gm_rd32(g, e + E_ROWS_SAVED), work_rows = gm_rd32(g, e + E_ROWS_WORK), screen_rows = gm_rd32(g, e + E_ROWS_SCREEN);
    uint32_t d4 = gm_rd16(g, e + E_W28), d3 = gm_rd16(g, e + E_W26);
    gm_wr32(g, e + E_LAST_DRAW, gm_rd32(g, 0x16A));

    /* 1. restore the background under every sprite that moved, was hidden or is about to be redrawn */
    for (uint32_t l = gm_rd32(g, e + E_FIRST_LIST); l; l = gm_rd32(g, l + L_NEXT))
        for (uint32_t s = gm_rd32(g, l + L_FIRST); s; s = gm_rd32(g, s + S_NEXT)) {
            if (!needs_erase(g, s)) continue;
            int16_t top = (int16_t)gm_rd16(g, s + 0x10), left = (int16_t)gm_rd16(g, s + 0x12);
            blit4(g, frame_code(g, s, S_FRAME_OLD, 0x12), row_ptr(g, saved_rows, top, left), row_ptr(g, work_rows, top, left), d4, d3);
        }

    /* 2. draw the changed visible sprites into the work buffer, and any sprite they touch */
    for (uint32_t l = gm_rd32(g, e + E_FIRST_LIST); l; l = gm_rd32(g, l + L_NEXT))
        for (uint32_t s = gm_rd32(g, l + L_FIRST); s; s = gm_rd32(g, s + S_NEXT)) {
            if (!gm_rd8(g, s + F_VISIBLE)) continue;
            if (!gm_rd8(g, s + F_DIRTY)) {
                if (!gm_rd8(g, l + L_ACTIVE)) continue;
                if (!touched_by_other(g, e, s)) continue;
            }
            blit2(g, frame_code(g, s, F_FRAME, 0xE), row_ptr(g, work_rows, (int16_t)gm_rd16(g, s + 8), (int16_t)gm_rd16(g, s + 0xA)), d3);
        }

    /* 3. copy the work buffer to the screen for the new and the old areas, clear the flags, drop removed sprites */
    int16_t off_h = (int16_t)gm_rd16(g, e + E_OFFSET_H), off_v = (int16_t)gm_rd16(g, e + E_OFFSET_V);
    d4 = gm_rd16(g, e + E_W24);
    for (uint32_t l = gm_rd32(g, e + E_FIRST_LIST); l; l = gm_rd32(g, l + L_NEXT)) {
        uint32_t s = gm_rd32(g, l + L_FIRST);
        while (s) {
            int16_t top = (int16_t)gm_rd16(g, s + 8), left = (int16_t)gm_rd16(g, s + 0xA);
            if (gm_rd8(g, s + F_DIRTY) && gm_rd8(g, s + F_VISIBLE)) {
                blit4(g, frame_code(g, s, F_FRAME, 0x12), row_ptr(g, work_rows, top, left),
                      row_ptr(g, screen_rows, (int16_t)(top + off_v), (int16_t)(left + off_h)), d3, d4);
            }
            if (gm_rd8(g, s + F_HIDDEN) | gm_rd8(g, s + F_DIRTY)) {
                int16_t otop = (int16_t)gm_rd16(g, s + 0x10), oleft = (int16_t)gm_rd16(g, s + 0x12);
                blit4(g, frame_code(g, s, S_FRAME_OLD, 0x12), row_ptr(g, work_rows, otop, oleft),
                      row_ptr(g, screen_rows, (int16_t)(otop + off_v), (int16_t)(oleft + off_h)), d3, d4);
            }
            gm_wr8(g, s + F_DIRTY, 0);
            gm_wr8(g, s + F_HIDDEN, 0);
            uint32_t done = s;
            s = gm_rd32(g, s + S_NEXT);
            if (gm_rd8(g, done + F_REMOVE)) engine_unlink(g, l, done);
        }
    }
    gm_wr8(g, e + E_DRAWN, 0xFF);
    gm_wr8(g, e + E_MOVED, 0);
}

/* ---- spawn and effect routines (seg3_3E26, 31A0, 3558, 363A, 3740, 3854) and the callbacks of the sprites they make ---- */
enum {
    G_LIST_EFFECTS = -0x63C, G_LIST_MUSHROOMS = -0x64C, G_CLASS_EXPLOSION = -0x610, G_CLASS_MUSHROOM = -0x638, G_CLASS_FRAGMENT = -0x61C,
    G_CLASS_SPARK = -0x60C, G_CLASS_DIGITS_A = -0x618, G_CLASS_DIGITS_B = -0x614, G_MUSH_LEVEL = -0x6E2
};
#define TAG_EXPL 0x6578706Cu
#define TAG_SHRM2 0x7368726Du
#define TAG_SCUM 0x7363756Du
#define TAG_FLAK 0x666C616Bu
#define TAG_SCOR 0x73636F72u

static void kill_sprite(GameCtx *g, uint32_t s)
{
    sprite_deactivate(g, s);
    gm_wr8(g, s + F_REMOVE, 1);
}

/* explosion frames: advance, and when past the last one the sprite is removed (seg3_4B6A) */
void frame_explosion(GameCtx *g, uint32_t s)
{
    int16_t f = (int16_t)(gm_rd16(g, s + F_FRAME) + 1);
    gm_wr16(g, s + F_FRAME, (uint16_t)f);
    if (f > (int16_t)gm_rd16(g, s + F_FRAMES)) {
        gm_wr16(g, s + F_FRAME, gm_rd16(g, s + F_FRAMES));
        kill_sprite(g, s);
    }
}

/* a score digit drifts up and to the side and lives 20 steps (seg3_472E) */
void move_score_digit(GameCtx *g, uint32_t s)
{
    gm_wr32(g, s + F_POS_V, gm_rd32(g, s + F_POS_V) - gm_rd32(g, s + F_VEL_V));
    gm_wr32(g, s + F_POS_H, gm_rd32(g, s + F_POS_H) - gm_rd32(g, s + F_VEL_H));
    gm_wr32(g, s + F_SCRATCH, gm_rd32(g, s + F_SCRATCH) + 1);
    if (gm_rd32(g, s + F_SCRATCH) == 0x14) kill_sprite(g, s);
    int32_t d7 = asr16(gm_rd32(g, s + F_POS_V)), d6 = asr16(gm_rd32(g, s + F_POS_H));
    if (w16(d7) < 0) { gm_wr32(g, s + F_POS_V, 0); kill_sprite(g, s); }
    if (w16(d6) < 0) { gm_wr32(g, s + F_POS_H, 0); kill_sprite(g, s); }
    if (w16(d6) > 0x1A8) { gm_wr32(g, s + F_POS_H, 0x1A0); kill_sprite(g, s); }
}

/* mushroom fragments (seg3_444EE move, seg3_444BA frame) */
void move_fragment(GameCtx *g, uint32_t s)
{
    gm_wr32(g, s + F_VEL_V, gm_rd32(g, s + F_VEL_V) + 0x8000u);
    gm_wr32(g, s + F_POS_H, gm_rd32(g, s + F_POS_H) + gm_rd32(g, s + F_VEL_H));
    gm_wr32(g, s + F_POS_V, gm_rd32(g, s + F_POS_V) + gm_rd32(g, s + F_VEL_V));
    int32_t d7 = asr16(gm_rd32(g, s + F_POS_H)), d6 = asr16(gm_rd32(g, s + F_POS_V));
    if (w16(d7) < 0 || w16(d6) < 0 || w16(d7) > 0x1AC || w16(d6) > 0x1DC) kill_sprite(g, s);
}

void frame_fragment(GameCtx *g, uint32_t s)
{
    int16_t f = (int16_t)(gm_rd16(g, s + F_FRAME) + 1);
    gm_wr16(g, s + F_FRAME, (uint16_t)f);
    if (f > 0xC) {
        gm_wr16(g, s + F_FRAME, 1);
        kill_sprite(g, s);
    }
}

/* sparks (seg3_46C8 move, seg3_469A frame) */
void move_spark(GameCtx *g, uint32_t s)
{
    gm_wr32(g, s + F_VEL_V, gm_rd32(g, s + F_VEL_V) + 0x2000u);
    gm_wr32(g, s + F_POS_H, gm_rd32(g, s + F_POS_H) + gm_rd32(g, s + F_VEL_H));
    gm_wr32(g, s + F_POS_V, gm_rd32(g, s + F_POS_V) + gm_rd32(g, s + F_VEL_V));
    int32_t d7 = asr16(gm_rd32(g, s + F_POS_H)), d6 = asr16(gm_rd32(g, s + F_POS_V));
    if (w16(d7) < 0 || w16(d6) < 0 || w16(d7) > 0x1A8 || w16(d6) > 0x1D8) kill_sprite(g, s);
}

void frame_spark(GameCtx *g, uint32_t s)
{
    int16_t f = (int16_t)(gm_rd16(g, s + F_FRAME) + 1);
    gm_wr16(g, s + F_FRAME, (uint16_t)f);
    if ((int32_t)f - (int32_t)gm_rd32(g, s + F_SCRATCH) - 6 > 0) gm_wr16(g, s + F_FRAME, gm_rd16(g, s + F_SCRATCH + 2));
}

static uint32_t new_sprite(GameCtx *g, int class_off, uint32_t tag, int list_off)
{
    return engine_sprite_alloc(g, gm_rd32(g, A5G(g, class_off)), tag, gm_rd32(g, A5G(g, list_off)));
}

/* seg3_3E26: an explosion where `src` was, using sprite class `cls`, kept inside the field */
void spawn_explosion(GameCtx *g, uint32_t src, uint32_t cls)
{
    uint32_t e = engine_sprite_alloc(g, cls, TAG_EXPL, gm_rd32(g, A5G(g, G_LIST_EFFECTS)));
    gm_wr32(g, e + F_POS_H, gm_rd32(g, src + F_POS_H));
    gm_wr32(g, e + F_POS_V, gm_rd32(g, src + F_POS_V));
    gm_wr32(g, e + S_MOVE_CB, g->a5 + 0x182);
    gm_wr32(g, e + S_FRAME_CB, g->a5 + 0x17A);
    gm_wr16(g, e + F_FRAME, 1);
    gm_wr8(g, e + F_VISIBLE, 1);
    gm_wr8(g, e + F_HIDDEN, 0);
    gm_wr8(g, e + F_DIRTY, 1);
    gm_wr16(g, e + S_FRAME_INTERVAL, 4);
    gm_wr32(g, e + 8, gm_rd32(g, src + 8));
    gm_wr32(g, e + 0xC, gm_rd32(g, src + 0xC));
    if (w16((int32_t)gm_rd16(g, e + 0xA) + (int16_t)gm_rd16(g, e + F_W)) > 0x1B0) {
        gm_wr16(g, e + 0xA, (uint16_t)(0x1B0 - (int16_t)gm_rd16(g, e + F_W)));
        gm_wr32(g, e + F_POS_H, shl16((int16_t)gm_rd16(g, e + 0xA)));
    }
    if (w16((int32_t)gm_rd16(g, e + 8) + (int16_t)gm_rd16(g, e + F_H)) > 0x1E0) {
        gm_wr16(g, e + 8, (uint16_t)(0x1E0 - (int16_t)gm_rd16(g, e + F_H)));
        gm_wr32(g, e + F_POS_V, shl16((int16_t)gm_rd16(g, e + 8)));
    }
}

/* seg3_31A0: a mushroom in cell [h][v] if the cell is inside the grid and empty */
void spawn_mushroom(GameCtx *g, int16_t h, int16_t v)
{
    if (h < 0 || v < 0 || h >= 0x1B || v >= 0x1E) return;
    uint32_t c = cell(g, h, v);
    if (gm_rd32(g, c)) return;
    int32_t hpx = (int32_t)(int16_t)(h << 4), vpx = (int32_t)(int16_t)(v << 4);
    uint32_t m = new_sprite(g, G_CLASS_MUSHROOM, TAG_SHRM2, G_LIST_MUSHROOMS);
    /* _OffsetRect(rect, hpx, vpx) */
    gm_wr16(g, m + 8, (uint16_t)(gm_rd16(g, m + 8) + vpx)); gm_wr16(g, m + 0xA, (uint16_t)(gm_rd16(g, m + 0xA) + hpx));
    gm_wr16(g, m + 0xC, (uint16_t)(gm_rd16(g, m + 0xC) + vpx)); gm_wr16(g, m + 0xE, (uint16_t)(gm_rd16(g, m + 0xE) + hpx));
    gm_wr32(g, m + 0x10, gm_rd32(g, m + 8));
    gm_wr32(g, m + 0x14, gm_rd32(g, m + 0xC));
    gm_wr32(g, m + F_POS_H, shl16(hpx));
    gm_wr32(g, m + F_POS_V, shl16(vpx));
    gm_wr32(g, m + S_MOVE_CB, g->a5 + 0xCA);
    gm_wr32(g, m + S_FRAME_CB, g->a5 + 0xD2);
    gm_wr16(g, m + S_FRAME_INTERVAL, 0xFFFF);
    gm_wr16(g, m + S_MOVE_INTERVAL, 0xFFFF);
    gm_wr8(g, m + F_DIRTY, 1);
    gm_wr16(g, m + F_FRAME, (uint16_t)((gm_rd16(g, A5G(g, G_MUSH_LEVEL)) << 2) + 5));
    gm_wr16(g, m + S_FRAME_OLD, gm_rd16(g, m + F_FRAME));
    gm_wr32(g, c, m);
    if (v >= 0x12) gm_wr16(g, A5G(g, G_MUSH_COUNT), (uint16_t)(gm_rd16(g, A5G(g, G_MUSH_COUNT)) + 1));
}

/* a random signed speed between 2 and 21 pixels per 16 steps, as the effects use */
static uint32_t random_speed(GameCtx *g, uint16_t range_unused_for_doc)
{
    (void)range_unused_for_doc;
    int32_t v = (int32_t)(int16_t)(rnd(g, 0x14) + 2) << 14;
    if (rnd(g, 2) == 0) v = -v;
    return (uint32_t)v;
}

/* seg3_3558: 1..5 mushroom fragments flying out from (a, b) */
void spawn_fragments(GameCtx *g, uint32_t a, uint32_t b)
{
    int16_t n = (int16_t)(rnd(g, 5) + 1);
    uint32_t h = a << 16, v = b << 16;
    while (n != 0) {
        uint32_t f = new_sprite(g, G_CLASS_FRAGMENT, TAG_SCUM, G_LIST_EFFECTS);
        gm_wr32(g, f + F_POS_H, h);
        gm_wr32(g, f + F_POS_V, v);
        gm_wr32(g, f + S_MOVE_CB, g->a5 + 0x14A);
        gm_wr32(g, f + S_FRAME_CB, g->a5 + 0x152);
        gm_wr8(g, f + F_VISIBLE, 1);
        gm_wr8(g, f + F_HIDDEN, 0);
        gm_wr16(g, f + 0xA, (uint16_t)a);
        gm_wr16(g, f + 8, (uint16_t)b);
        gm_wr32(g, f + F_VEL_H, random_speed(g, 0));
        gm_wr32(g, f + F_VEL_V, random_speed(g, 0));
        gm_wr8(g, f + F_DIRTY, 1);
        n--;
    }
}

/* seg3_363A: 10..29 sparks from the position of `src` */
void spawn_sparks(GameCtx *g, uint32_t src)
{
    int16_t n = (int16_t)(10 + rnd(g, 0x14));
    while (n != 0) {
        uint32_t f = new_sprite(g, G_CLASS_SPARK, TAG_FLAK, G_LIST_EFFECTS);
        gm_wr32(g, f + F_POS_H, gm_rd32(g, src + F_POS_H));
        gm_wr32(g, f + F_POS_V, gm_rd32(g, src + F_POS_V));
        gm_wr16(g, f + 0xA, (uint16_t)asr16(gm_rd32(g, f + F_POS_H)));
        gm_wr16(g, f + 8, (uint16_t)asr16(gm_rd32(g, f + F_POS_V)));
        gm_wr32(g, f + S_MOVE_CB, g->a5 + 0x18A);
        gm_wr32(g, f + S_FRAME_CB, g->a5 + 0x192);
        gm_wr8(g, f + F_VISIBLE, 1);
        gm_wr8(g, f + F_HIDDEN, 0);
        gm_wr16(g, f + F_FRAME, (uint16_t)((int16_t)rnd(g, 0x10) * 6 + 1));
        gm_wr32(g, f + F_SCRATCH, (uint32_t)(int32_t)(int16_t)gm_rd16(g, f + F_FRAME));
        gm_wr32(g, f + F_VEL_H, random_speed(g, 0));
        gm_wr32(g, f + F_VEL_V, random_speed(g, 0));
        gm_wr8(g, f + F_DIRTY, 1);
        n--;
    }
}

/* seg3_3740 (digit class A) and seg3_3854 (digit class B): the number `value` as digit sprites that float up from `src` */
void spawn_score_digits(GameCtx *g, uint32_t src, int16_t value, int class_b)
{
    int16_t digits[8];
    int16_t n = 0, d5 = value;
    while (d5 > 0) {
        int16_t q = (int16_t)(d5 / 10);
        digits[n++] = (int16_t)(d5 - q * 10 + 1);
        d5 = q;
    }
    int32_t spread = (int32_t)(int16_t)(((n - 1) >> 1)) * 2;
    if (n % 2 == 0) spread += 1;
    for (int16_t i = n; i > 0; i--) {
        uint32_t f = new_sprite(g, class_b ? G_CLASS_DIGITS_B : G_CLASS_DIGITS_A, TAG_SCOR, G_LIST_EFFECTS);
        gm_wr32(g, f + F_POS_H, ((uint32_t)(int32_t)(int16_t)gm_rd16(g, src + F_W) << 15) + gm_rd32(g, src + F_POS_H));
        gm_wr32(g, f + F_POS_V, ((uint32_t)(int32_t)(int16_t)gm_rd16(g, src + F_H) << 15) + gm_rd32(g, src + F_POS_V));
        gm_wr16(g, f + 0xA, (uint16_t)asr16(gm_rd32(g, f + F_POS_H)));
        gm_wr16(g, f + 8, (uint16_t)asr16(gm_rd32(g, f + F_POS_V)));
        gm_wr32(g, f + S_MOVE_CB, g->a5 + 0x172);
        gm_wr32(g, f + S_FRAME_CB, g->a5 + 0x16A);
        gm_wr16(g, f + F_FRAME, (uint16_t)digits[i - 1]);
        gm_wr8(g, f + F_VISIBLE, 1);
        gm_wr8(g, f + F_HIDDEN, 0);
        gm_wr32(g, f + F_SCRATCH, 0);
        gm_wr32(g, f + F_VEL_H, shl16(spread));
        gm_wr32(g, f + F_VEL_V, 0x10000);
        gm_wr8(g, f + F_DIRTY, 1);
        spread -= 2;
    }
}

/* ---- the shooter, its shots, coins and the shot hit handler (seg3_5216, 54C8, 54DA, 2256, 557A, 3968, 3EF4, 454C) ---- */
enum {
    G_PENDING_X = -0x41C, G_PENDING_Y = -0x41A, G_SHOOTER_FRAME = -0x674, G_SHOOTER_H_MAX = -0x65E, G_SHOOTER_H_MIN = -0x662,
    G_SHOOTER_V_MAX = -0x660, G_SHOOTER_V_MIN = -0x664, G_PASS_THROUGH = -0x66C, G_GUIDED = -0x66D, G_SHOT_TOP = -0x654,
    G_SHOTS_IN_FLIGHT = -0x6CC, G_SHOTS_ALLOWED = -0x6CA, G_LAST_SHOT = -0x6C8, G_SHOT_DELAY = -0x6C4, G_BUTTON = -0x418,
    G_MACHINE_GUN = -0x66B, G_LIST_SHOTS = -0x644, G_CLASS_SHOT = -0x62C, G_LEVEL = -0x734, G_MULT = -0x704, G_POINTS = -0x706,
    G_PEDE_ALIVE = -0x6E6, G_FLEA_POINTS = -0x6C0, G_FLEA_TICK = -0x6BA, G_MULT_SEEN = -0x6AA, G_REDRAW = -0x66E,
    G_PSYC_TICK_A = -0x714, G_PSYC_TICK_B = -0x710, G_PSYC_FLAG = -0x716, G_SCORP_TICK = -0x6A6, G_CLASS_COIN = -0x600,
    S_SHOT_POOF = -0x580, S_FALLING_POISON = -0x544, S_SPECIAL = -0x530, S_COIN_SPAWN = -0x564, S_SCORE_HIT = -0x57C,
    S_FLEA_HIT = -0x574, S_FLEA_DIE = -0x570, S_MULT = -0x540, S_GECKO_HIT = -0x510, S_PSYC = -0x504, S_COIN_HIT = -0x528,
    S_FODDER_A = -0x514, S_FODDER_B = -0x52C, S_ROOT_KILL = -0x50C, S_ROOT_HIT = -0x55C, S_SCORPION = -0x550,
    S_FIRE_GUN = -0x500, S_FIRE = -0x584,
    C_FLEA = -0x604, C_GECKO = -0x5E8, C_SCORPION = -0x5FC, C_EXPL_MUSH = -0x608, C_EXPL_FODDER = -0x610
};
#define TAG_FLEA 0x666C6561u
#define TAG_MULT 0x6D756C74u
#define TAG_GECK 0x6765636Bu
#define TAG_PSYC 0x70737963u
#define TAG_YUMY 0x79756D79u
#define TAG_FODR 0x666F6472u
#define TAG_ROOT 0x726F6F74u
#define TAG_SHOT 0x73686F74u

/* the shooter: mouse movement, banking frame and the walls (seg3_5216) */
void move_shooter(GameCtx *g, uint32_t s)
{
    gm_wr8(g, A5G(g, -0x729), 1);
    int32_t d5 = asr16(gm_rd32(g, s + F_POS_H)), d4 = asr16(gm_rd32(g, s + F_POS_V));
    int32_t d7 = (int32_t)(int16_t)((int16_t)gm_rd16(g, A5G(g, G_PENDING_X)) >> 1);
    if (d7 > 13) d7 = 13;
    if (d7 < -13) d7 = -13;
    gm_wr16(g, A5G(g, G_PENDING_X), (uint16_t)(gm_rd16(g, A5G(g, G_PENDING_X)) - (uint16_t)d7));
    int32_t d6 = (int32_t)(int16_t)((int16_t)gm_rd16(g, A5G(g, G_PENDING_Y)) >> 1);
    if (d6 > 13) d6 = 13;
    if (d6 < -13) d6 = -13;
    gm_wr16(g, A5G(g, G_PENDING_Y), (uint16_t)(gm_rd16(g, A5G(g, G_PENDING_Y)) - (uint16_t)d6));
    d5 += d7; d4 += d6;
    uint16_t fr = (uint16_t)(ldiv32(d7, 2) + (int32_t)gm_rd16(g, s + F_FRAME));
    gm_wr16(g, A5G(g, G_SHOOTER_FRAME), fr);
    if ((int16_t)(fr - 1) < 0) gm_wr16(g, A5G(g, G_SHOOTER_FRAME), (uint16_t)(gm_rd16(g, A5G(g, G_SHOOTER_FRAME)) + gm_rd16(g, s + F_FRAMES)));
    if ((int16_t)gm_rd16(g, A5G(g, G_SHOOTER_FRAME)) > (int16_t)gm_rd16(g, s + F_FRAMES))
        gm_wr16(g, A5G(g, G_SHOOTER_FRAME), (uint16_t)(gm_rd16(g, A5G(g, G_SHOOTER_FRAME)) - gm_rd16(g, s + F_FRAMES)));
    for (int pass = 0; pass < 2; pass++) {
        if ((int16_t)gm_rd16(g, A5G(g, G_SHOOTER_H_MAX)) < d5) { gm_wr16(g, A5G(g, G_PENDING_X), 0); d5 = (int16_t)gm_rd16(g, A5G(g, G_SHOOTER_H_MAX)); }
        if ((int16_t)gm_rd16(g, A5G(g, G_SHOOTER_H_MIN)) > d5) { gm_wr16(g, A5G(g, G_PENDING_X), 0); d5 = (int16_t)gm_rd16(g, A5G(g, G_SHOOTER_H_MIN)); }
        if ((int16_t)gm_rd16(g, A5G(g, G_SHOOTER_V_MAX)) < d4) { gm_wr16(g, A5G(g, G_PENDING_Y), 0); d4 = (int16_t)gm_rd16(g, A5G(g, G_SHOOTER_V_MAX)); }
        if ((int16_t)gm_rd16(g, A5G(g, G_SHOOTER_V_MIN)) > d4) { gm_wr16(g, A5G(g, G_PENDING_Y), 0); d4 = (int16_t)gm_rd16(g, A5G(g, G_SHOOTER_V_MIN)); }
        if (pass == 0) {
            if (gm_rd8(g, A5G(g, G_PASS_THROUGH))) continue;      /* pass through: skip the mushroom checks, clamp once more */
            if (d7 < 0) {
                if (gm_rd32(g, cell32(g, G_GRID, d5 >> 4, (d4 + 4) >> 4)) || gm_rd32(g, cell32(g, G_GRID, d5 >> 4, (d4 + 0xC) >> 4))) {
                    d5 = (((d5 >> 4) + 1) << 4) - 2; gm_wr16(g, A5G(g, G_PENDING_X), 0);
                }
            }
            if (d7 > 0) {
                if (gm_rd32(g, cell32(g, G_GRID, (d5 >> 4) + 1, (d4 + 4) >> 4)) || gm_rd32(g, cell32(g, G_GRID, (d5 >> 4) + 1, (d4 + 0xC) >> 4))) {
                    d5 = ((d5 >> 4) << 4) + 2; gm_wr16(g, A5G(g, G_PENDING_X), 0);
                }
            }
            if (d6 < 0) {
                if (gm_rd32(g, cell32(g, G_GRID, (d5 + 4) >> 4, d4 >> 4)) || gm_rd32(g, cell32(g, G_GRID, (d5 + 0xC) >> 4, d4 >> 4))) {
                    d4 = (((d4 >> 4) + 1) << 4) - 2; gm_wr16(g, A5G(g, G_PENDING_Y), 0);
                }
            }
            if (d6 > 0) {
                if (gm_rd32(g, cell32(g, G_GRID, (d5 + 4) >> 4, (d4 >> 4) + 1)) || gm_rd32(g, cell32(g, G_GRID, (d5 + 0xC) >> 4, (d4 >> 4) + 1))) {
                    d4 = ((d4 >> 4) << 4) + 2; gm_wr16(g, A5G(g, G_PENDING_Y), 0);
                }
            }
        }
    }
    gm_wr32(g, s + F_POS_H, shl16(d5));
    gm_wr32(g, s + F_POS_V, shl16(d4));
}

void frame_shooter(GameCtx *g, uint32_t s)                                  /* seg3_54C8 */
{
    gm_wr16(g, s + F_FRAME, gm_rd16(g, A5G(g, G_SHOOTER_FRAME)));
}

/* a shot flies 16 px up; guided shots follow the shooter (seg3_54DA) */
void move_shot(GameCtx *g, uint32_t s)
{
    if (gm_rd8(g, A5G(g, G_GUIDED))) gm_wr32(g, s + F_POS_H, gm_rd32(g, gm_rd32(g, A5G(g, G_PLAYER)) + F_POS_H) + 0x80000u);
    int32_t d7 = asr16(gm_rd32(g, s + F_POS_H));
    int32_t d6 = asr16(gm_rd32(g, s + F_POS_V)) - 0x10;
    if ((int32_t)(int16_t)gm_rd16(g, A5G(g, G_SHOT_TOP)) > d6) {
        d6 = (int16_t)gm_rd16(g, A5G(g, G_SHOT_TOP));
        if (gm_rd32(g, s + S_OLD_V) == 0) {
            sprite_deactivate(g, s);
            gm_wr8(g, s + F_REMOVE, 1);
            gm_wr16(g, A5G(g, G_SHOTS_IN_FLIGHT), (uint16_t)(gm_rd16(g, A5G(g, G_SHOTS_IN_FLIGHT)) - 1));
        }
    }
    gm_wr32(g, s + F_POS_H, shl16(d7));
    gm_wr32(g, s + F_POS_V, shl16(d6));
}

/* seg3_2256: fire a shot if the button is down, fewer than allowed are in flight and the delay has passed */
void fire_shot(GameCtx *g)
{
    if ((int16_t)gm_rd16(g, A5G(g, G_SHOTS_IN_FLIGHT)) >= (int16_t)gm_rd16(g, A5G(g, G_SHOTS_ALLOWED))) return;
    if (!((int32_t)(gm_rd32(g, A5G(g, G_TICKS)) - gm_rd32(g, A5G(g, G_LAST_SHOT))) > (int32_t)gm_rd32(g, A5G(g, G_SHOT_DELAY)))) return;
    if (!gm_rd8(g, A5G(g, G_BUTTON))) return;
    gm_wr16(g, A5G(g, G_SHOTS_IN_FLIGHT), (uint16_t)(gm_rd16(g, A5G(g, G_SHOTS_IN_FLIGHT)) + 1));
    gm_wr32(g, A5G(g, G_LAST_SHOT), gm_rd32(g, A5G(g, G_TICKS)));
    uint32_t s = new_sprite(g, G_CLASS_SHOT, TAG_SHOT, G_LIST_SHOTS);
    gm_wr32(g, s + S_MOVE_CB, g->a5 + 0xFA);
    gm_wr32(g, s + S_FRAME_CB, g->a5 + 0x102);
    uint32_t pl = gm_rd32(g, A5G(g, G_PLAYER));
    gm_wr32(g, s + F_POS_H, (8u << 16) + gm_rd32(g, pl + F_POS_H));
    gm_wr32(g, s + F_POS_V, gm_rd32(g, pl + F_POS_V) - 0x50000u);
    gm_wr16(g, s + 8, (uint16_t)asr16(gm_rd32(g, s + F_POS_V)));
    gm_wr16(g, s + 0xA, (uint16_t)asr16(gm_rd32(g, s + F_POS_H)));
    call_sound(g, gm_rd8(g, A5G(g, G_MACHINE_GUN)) ? S_FIRE_GUN : S_FIRE, 2);
}

/* a coin falling and bouncing (seg3_454C move; its frame routine is the plain one) */
void move_yummy(GameCtx *g, uint32_t s)
{
    if (gm_rd32(g, s + F_VEL_H)) gm_wr16(g, s + F_VEL_V, (uint16_t)(gm_rd16(g, s + F_VEL_V) + 1));
    gm_wr32(g, s + F_POS_V, gm_rd32(g, s + F_POS_V) + gm_rd32(g, s + F_VEL_V));
    gm_wr32(g, s + F_POS_H, gm_rd32(g, s + F_POS_H) + gm_rd32(g, s + F_VEL_H));
    int32_t d7 = asr16(gm_rd32(g, s + F_POS_V)), d6 = asr16(gm_rd32(g, s + F_POS_H));
    if (w16((int32_t)gm_rd16(g, s + F_H) + w16(d7)) >= 0x1E0) {
        gm_wr32(g, s + F_POS_V, shl16(0x1E0 - (int16_t)gm_rd16(g, s + F_H)));
        if (gm_rd32(g, s + F_VEL_H) == 0) {
            kill_sprite(g, s);
            call_sound(g, S_LEAVE, 2);
        } else {
            int16_t d5 = (int16_t)asr16(gm_rd32(g, s + F_VEL_V));
            if (d5 < 4 && d5 >= 0) {
                kill_sprite(g, s);
                call_sound(g, S_LEAVE, 2);
            } else {
                d5 = (int16_t)(d5 - 1);
                gm_wr32(g, s + F_VEL_V, shl16(-(int32_t)d5));
                call_sound(g, -0x528, 2);
            }
        }
    }
    if (w16(d7) < 0) gm_wr32(g, s + F_POS_V, 0);
    if (w16(d6) < 0) { kill_sprite(g, s); call_sound(g, S_LEAVE, 2); }
    if (w16(d6) > 0x1A0) { kill_sprite(g, s); call_sound(g, S_LEAVE, 2); }
}

/* seg3_3968: a mushroom turns into a bouncing "root" */
void spawn_root(GameCtx *g, uint32_t src)
{
    uint32_t r = new_sprite(g, G_CLASS_MUSHROOM, TAG_ROOT, -0x640);
    gm_wr32(g, r + F_POS_H, gm_rd32(g, src + F_POS_H));
    gm_wr32(g, r + F_POS_V, gm_rd32(g, src + F_POS_V));
    gm_wr16(g, r + 0xA, (uint16_t)asr16(gm_rd32(g, r + F_POS_H)));
    gm_wr16(g, r + 8, (uint16_t)asr16(gm_rd32(g, r + F_POS_V)));
    gm_wr32(g, r + S_MOVE_CB, g->a5 + 0x1AA);
    gm_wr32(g, r + S_FRAME_CB, g->a5 + 0x1B2);
    gm_wr16(g, r + F_FRAME, gm_rd16(g, src + F_FRAME));
    gm_wr8(g, r + F_VISIBLE, 1);
    gm_wr8(g, r + F_HIDDEN, 0);
    gm_wr32(g, r + F_SCRATCH, 0);
    int32_t vx = (int32_t)(int16_t)(10 + rnd(g, 0x1E)) << 12;
    if (rnd(g, 2) == 0) vx = -vx;
    gm_wr32(g, r + F_VEL_H, (uint32_t)vx);
    int32_t vy = (10 + (int32_t)(int16_t)rnd(g, 0x14)) << 15;
    gm_wr32(g, r + F_VEL_V, (uint32_t)(-vy));
    gm_wr8(g, r + F_DIRTY, 1);
}

/* seg3_3EF4: one coin (or a burst of 3..15) from `src`; some fly out sideways */
void spawn_coins(GameCtx *g, uint32_t src, uint32_t cls)
{
    int spread = 0;
    if (rnd(g, 7) == 1) spread = 1;
    int16_t n = 1;
    if (rnd(g, 0x14) == 1) { n = (int16_t)(rnd(g, 0xD) + 3); spread = 1; }
    while (n != 0) {
        uint32_t y = engine_sprite_alloc(g, cls, TAG_YUMY, gm_rd32(g, A5G(g, -0x640)));
        gm_wr32(g, y + F_POS_H, gm_rd32(g, src + F_POS_H));
        gm_wr32(g, y + F_POS_V, gm_rd32(g, src + F_POS_V));
        gm_wr32(g, y + S_MOVE_CB, g->a5 + 0x19A);
        gm_wr32(g, y + S_FRAME_CB, g->a5 + 0x1A2);
        gm_wr16(g, y + F_FRAME, (uint16_t)(rnd(g, gm_rd16(g, y + F_FRAMES)) + 1));
        gm_wr8(g, y + F_VISIBLE, 1);
        gm_wr8(g, y + F_HIDDEN, 0);
        gm_wr8(g, y + F_DIRTY, 1);
        if (spread) {
            int32_t vx = (int32_t)(int16_t)rnd(g, 0x46) << 12;
            if (rnd(g, 2) == 0) vx = -vx;
            if (g->fix_coin && vx == 0) vx = 1 << 12;                    /* a sideways speed of 0 while flying up leaves the coin without gravity: it rises to the top and stays (original bug) */
            gm_wr32(g, y + F_VEL_H, (uint32_t)vx);
            int32_t vy = (10 + (int32_t)(int16_t)rnd(g, 0x14)) << 15;
            gm_wr32(g, y + F_VEL_V, (uint32_t)(-vy));
        } else {
            gm_wr32(g, y + F_VEL_V, 0x40000u);
            gm_wr32(g, y + F_VEL_H, 0);
        }
        gm_wr32(g, y + 8, gm_rd32(g, src + 8));
        gm_wr32(g, y + 0xC, gm_rd32(g, src + 0xC));
        n--;
    }
}

/* seg3_4010: a mushroom hit at a high level lets loose a falling bullet from the shot's place */
void spawn_special_shot(GameCtx *g, uint32_t src)
{
    uint32_t s = new_sprite(g, -0x5F4, TAG_SHOT, -0x640);
    gm_wr32(g, s + F_POS_H, gm_rd32(g, src + F_POS_H));
    gm_wr32(g, s + F_POS_V, gm_rd32(g, src + F_POS_V));
    gm_wr32(g, s + S_MOVE_CB, g->a5 + 0x10A);
    gm_wr32(g, s + S_FRAME_CB, g->a5 + 0x112);
    gm_wr16(g, s + F_FRAME, 1);
    gm_wr8(g, s + F_VISIBLE, 1);
    gm_wr8(g, s + F_HIDDEN, 0);
    gm_wr8(g, s + F_DIRTY, 1);
    gm_wr32(g, s + F_VEL_V, ((uint32_t)(int32_t)(int16_t)rnd(g, 6) << 16) + 0x80000u);
    gm_wr32(g, s + 8, gm_rd32(g, src + 8));
    gm_wr32(g, s + 0xC, gm_rd32(g, src + 0xC));
}

static void drop_shot(GameCtx *g, uint32_t shot)
{
    sprite_deactivate(g, shot);
    gm_wr16(g, A5G(g, G_SHOTS_IN_FLIGHT), (uint16_t)(gm_rd16(g, A5G(g, G_SHOTS_IN_FLIGHT)) - 1));
    gm_wr8(g, shot + F_REMOVE, 1);
}

static void add_score(GameCtx *g, int32_t pts)
{
    gm_wr32(g, A5G(g, G_SCORE), gm_rd32(g, A5G(g, G_SCORE)) + (uint32_t)pts);
}

/* the score pop-up value is a 16-bit multiple of the multiplier */
static int16_t popup_value(GameCtx *g, uint16_t base)
{
    return (int16_t)(uint16_t)((uint32_t)base * gm_rd16(g, A5G(g, G_MULT)));
}

/* seg3_557A: what a shot hit. `consume` = the shot is used up by the hit */
void shot_hit(GameCtx *g, uint32_t shot, int consume)
{
    int32_t d4 = asr16(gm_rd32(g, shot + F_POS_H)), d3 = asr16(gm_rd32(g, shot + F_POS_V));
    int32_t cellh = d4 >> 4;
    int16_t d6 = (int16_t)cellh;
    int32_t d5 = (0xF + d3) >> 4;
    uint32_t a3 = gm_rd32(g, (uint32_t)((int32_t)d6 * 0x78 + (int32_t)gm_rd32(g, A5G(g, G_GRID)) + ((int32_t)(int16_t)d5 << 2)));
    if (d6 >= 0x1B || (int16_t)d5 >= 0x1E) a3 = 0;
    if (!a3) {
        d6 = (int16_t)cellh;
        d5 = (d3 + 7) >> 4;
        a3 = gm_rd32(g, (uint32_t)((int32_t)d6 * 0x78 + (int32_t)gm_rd32(g, A5G(g, G_GRID)) + ((int32_t)(int16_t)d5 << 2)));
        if (d6 >= 0x1B || (int16_t)d5 >= 0x1E) a3 = 0;
    }
    if (a3 && gm_rd8(g, shot + F_VISIBLE)) {
        /* a mushroom is hit */
        spawn_fragments(g, (uint32_t)d4, (uint32_t)d3);
        if (consume) drop_shot(g, shot);
        call_sound(g, S_SHOT_POOF, 2);
        int16_t frame = (int16_t)gm_rd16(g, a3 + F_FRAME);
        if (frame % 4 != 0) {
            int16_t level = (int16_t)gm_rd16(g, A5G(g, G_LEVEL));
            int32_t odds = 0x32 - level;
            if ((int16_t)odds < 10) odds = 10;
            if (frame % 4 == 1) {
                if (rnd(g, (uint16_t)odds) == 1 && level > 0xB) {
                    call_sound(g, S_FALLING_POISON, 3);
                    spawn_root(g, a3);
                    grid_remove_mushroom(g, d6, (int16_t)d5);
                    return;
                }
            }
            odds = 0x46 - level;
            if ((int16_t)odds < 0x14) odds = 0x14;
            if (rnd(g, (uint16_t)odds) == 1 && level > 0x16 && (int16_t)d5 <= 0x12) {
                call_sound(g, S_SPECIAL, 3);
                spawn_special_shot(g, shot);
                return;
            }
            gm_wr16(g, a3 + S_FRAME_OLD, gm_rd16(g, a3 + F_FRAME));
            gm_wr16(g, a3 + F_FRAME, (uint16_t)(gm_rd16(g, a3 + F_FRAME) + 1));
            gm_wr8(g, a3 + F_DIRTY, 1);
            gm_wr8(g, a3 + F_HIDDEN, 1);
            return;
        }
        if (!gm_rd8(g, a3 + F_VISIBLE)) return;
        if (rnd(g, 0xF) == 0) {
            call_sound(g, S_COIN_SPAWN, 3);
            spawn_coins(g, a3, gm_rd32(g, A5G(g, G_CLASS_COIN)));
        }
        uint16_t pts = gm_rd16(g, A5G(g, G_MULT));
        if ((int16_t)gm_rd16(g, a3 + F_FRAME) < 5) pts = (uint16_t)(5u * gm_rd16(g, A5G(g, G_MULT)));
        spawn_score_digits(g, a3, (int16_t)pts, 0);
        grid_remove_mushroom(g, d6, (int16_t)d5);
        add_score(g, (int32_t)gm_rd32(g, A5G(g, G_POINTS)));
        return;
    }

    /* not a mushroom: a pede segment? */
    a3 = engine_overlap(g, shot, gm_rd32(g, A5G(g, G_PEDE_LIST)));
    if (a3) {
        if (gm_rd32(g, a3) == TAG_PHED) {
            add_score(g, lmul((int32_t)gm_rd32(g, A5G(g, G_POINTS)), 0x64));
            spawn_score_digits(g, a3, popup_value(g, 0x64), 0);
        } else {
            add_score(g, lmul((int32_t)gm_rd32(g, A5G(g, G_POINTS)), 10));
            spawn_score_digits(g, a3, popup_value(g, 10), 0);
        }
        spawn_explosion(g, a3, gm_rd32(g, A5G(g, C_EXPL_MUSH)));
        call_sound(g, S_SCORE_HIT, 2);
        int32_t h = (asr16(gm_rd32(g, a3 + F_POS_H)) + 8) >> 4, v = (asr16(gm_rd32(g, a3 + F_POS_V)) + 8) >> 4;
        if ((int16_t)h < 0) h = 0;
        if ((int16_t)v < 0) v = 0;
        if ((int16_t)h >= 0x1B) h = 0x1A;
        if ((int16_t)v >= 0x1E) v = 0x1D;
        int32_t off_h = (int32_t)(int16_t)h * 0x78, off_v = (int32_t)(int16_t)v << 2;
        if (gm_rd32(g, (uint32_t)(off_h + (int32_t)gm_rd32(g, A5G(g, G_GRID)) + off_v)) == 0) spawn_mushroom(g, (int16_t)h, (int16_t)v);
        if (consume) drop_shot(g, shot);
        uint32_t rec = gm_rd32(g, a3 + F_SCRATCH);
        uint32_t ahead = gm_rd32(g, rec + R_AHEAD);
        if (ahead) gm_wr32(g, gm_rd32(g, ahead + F_SCRATCH) + R_BEHIND, 0);
        uint32_t behind = gm_rd32(g, rec + R_BEHIND);
        if (behind) {
            int32_t by = asr16(gm_rd32(g, behind + F_POS_V));
            if (by >= 0x1E0) by = 0x1D0;
            gm_wr32(g, behind + F_POS_V, shl16(by));
            uint32_t brec = gm_rd32(g, behind + F_SCRATCH);
            gm_wr32(g, behind, TAG_PHED);
            if (gm_rd8(g, rec + R_DIVE)) { gm_wr8(g, brec + R_DIR, 4); gm_wr8(g, brec + R_NEXT, 2); }
            gm_wr16(g, brec + R_TARGET, (uint16_t)(((uint16_t)((by >> 4) << 4)) + 0x10));
            if ((int16_t)gm_rd16(g, brec + R_TARGET) >= 0x1E0) gm_wr16(g, brec + R_TARGET, 0x1D0);
        }
        sprite_deactivate(g, a3);
        gm_wr32(g, (uint32_t)((int32_t)gm_rd32(g, A5G(g, G_PEDE_GRID)) + off_h + off_v), 0);
        gm_wr16(g, A5G(g, G_PEDE_ALIVE), (uint16_t)(gm_rd16(g, A5G(g, G_PEDE_ALIVE)) - 1));
        return;
    }

    /* the other creatures */
    a3 = engine_overlap(g, shot, gm_rd32(g, A5G(g, G_LIST_BAD)));
    if (!a3) return;
    uint32_t tag = gm_rd32(g, a3);
    if (tag == TAG_FLEA) {
        if (consume) drop_shot(g, shot);
        int16_t hits = (int16_t)(gm_rd32(g, a3 + F_SCRATCH) >> 16);
        if (hits < 1) {
            hits = (int16_t)(hits + 1);
            gm_wr32(g, a3 + F_SCRATCH, ((uint32_t)(int32_t)hits << 16) + (gm_rd32(g, a3 + F_SCRATCH) & 0xFFFF));
            call_sound(g, S_FLEA_HIT, 2);
            if (gm_rd32(g, a3 + F_VEL_H) == 0) gm_wr32(g, a3 + F_VEL_V, 0x80000u);
        } else {
            call_sound(g, S_FLEA_DIE, 3);
            call_sound(g, S_SCORE_HIT, 2);
            add_score(g, lmul((int32_t)gm_rd32(g, A5G(g, G_POINTS)), (int16_t)gm_rd16(g, A5G(g, G_FLEA_POINTS))));
            spawn_explosion(g, a3, gm_rd32(g, A5G(g, C_FLEA)));
            spawn_score_digits(g, a3, popup_value(g, gm_rd16(g, A5G(g, G_FLEA_POINTS))), 0);
            sprite_deactivate(g, a3);
            gm_wr16(g, A5G(g, G_FLEA_COUNT), (uint16_t)(gm_rd16(g, A5G(g, G_FLEA_COUNT)) - 1));
            gm_wr8(g, a3 + F_REMOVE, 1);
            gm_wr16(g, A5G(g, G_FLEA_POINTS), (uint16_t)(gm_rd16(g, A5G(g, G_FLEA_POINTS)) << 1));
            gm_wr32(g, A5G(g, G_FLEA_TICK), gm_rd32(g, A5G(g, G_TICKS)) + 10);
        }
    }
    if (tag == TAG_MULT) {
        call_sound(g, S_MULT, 3);
        gm_wr16(g, A5G(g, G_MULT_SEEN), (uint16_t)(1 + gm_rd16(g, a3 + F_FRAME)));
        gm_wr8(g, A5G(g, G_REDRAW), 1);
        if (consume) drop_shot(g, shot);
        sprite_deactivate(g, a3);
    }
    if (tag == TAG_GECK) {
        call_sound(g, S_GECKO_HIT, 3);
        if (consume) drop_shot(g, shot);
        add_score(g, lmul((int32_t)gm_rd32(g, A5G(g, G_POINTS)), 0x3E8));
        spawn_explosion(g, a3, gm_rd32(g, A5G(g, C_GECKO)));
        spawn_score_digits(g, a3, (int16_t)(uint16_t)(0x3E8u * gm_rd16(g, A5G(g, G_MULT))), 0);
        sprite_deactivate(g, a3);
    }
    if (tag == TAG_PSYC) {
        if (consume) drop_shot(g, shot);
        gm_wr32(g, A5G(g, G_PSYC_TICK_A), gm_rd32(g, A5G(g, G_TICKS)));
        gm_wr32(g, A5G(g, G_PSYC_TICK_B), gm_rd32(g, A5G(g, G_TICKS)));
        gm_wr8(g, A5G(g, G_PSYC_FLAG), 1);
        if (gm_rd8(g, A5G(g, G_MUSIC))) { uint8_t w[2]; put16(w, 4); ext(g, (int)(g->a5 + 0x3DA), w, 2); }
        gm_wr32(g, A5G(g, G_POINTS), 10);
        sprite_deactivate(g, a3);
        call_sound(g, S_PSYC, 3);
    }
    if (tag == TAG_YUMY) {
        call_sound(g, S_COIN_HIT, 3);
        gm_wr32(g, a3 + F_VEL_V, 0xFFF60000u);
        int32_t vx = (int32_t)(int16_t)(10 + rnd(g, 0x28)) << 12;
        gm_wr32(g, a3 + F_VEL_H, (uint32_t)vx);
        if (rnd(g, 2) == 0) gm_wr32(g, a3 + F_VEL_H, 0u - gm_rd32(g, a3 + F_VEL_H));
        if (consume) drop_shot(g, shot);
    }
    if (tag == TAG_FODR) {
        int32_t d = (int32_t)(int16_t)(int16_t)((rnd(g, 6) + 1) * 0x1F4);
        spawn_score_digits(g, a3, (int16_t)d, 1);
        spawn_explosion(g, a3, gm_rd32(g, A5G(g, C_EXPL_FODDER)));
        call_sound(g, S_FODDER_A, 3);
        call_sound(g, S_FODDER_B, 3);
        gm_wr32(g, A5G(g, G_BONUS), gm_rd32(g, A5G(g, G_BONUS)) + (uint32_t)d);
        sprite_deactivate(g, a3);
        gm_wr8(g, a3 + F_REMOVE, 1);
        if (consume) drop_shot(g, shot);
    }
    if (tag == TAG_ROOT) {
        gm_wr32(g, a3 + F_SCRATCH, gm_rd32(g, a3 + F_SCRATCH) + 1);
        if (2 < (int32_t)gm_rd32(g, a3 + F_SCRATCH)) {
            call_sound(g, S_ROOT_KILL, 3);
            int32_t d = lmul((int32_t)gm_rd32(g, A5G(g, G_POINTS)), 0xBB8);
            if ((int16_t)gm_rd16(g, a3 + F_FRAME) < 5) d += lmul((int32_t)gm_rd32(g, A5G(g, G_POINTS)), 0x1770);
            add_score(g, d);
            spawn_score_digits(g, a3, (int16_t)d, 0);
            sprite_deactivate(g, a3);
            gm_wr8(g, a3 + F_REMOVE, 1);
        } else {
            call_sound(g, S_ROOT_HIT, 3);
            gm_wr32(g, a3 + F_VEL_V, 0xFFFB0000u);
            int32_t vx = (int32_t)(int16_t)(10 + rnd(g, 0x1E)) << 12;
            gm_wr32(g, a3 + F_VEL_H, (uint32_t)vx);
            if (rnd(g, 2) == 0) gm_wr32(g, a3 + F_VEL_H, 0u - gm_rd32(g, a3 + F_VEL_H));
        }
        if (consume) drop_shot(g, shot);
    }
    if (tag == TAG_SCOR) {
        call_sound(g, S_SCORPION, 3);
        call_sound(g, S_SCORE_HIT, 2);
        spawn_explosion(g, a3, gm_rd32(g, A5G(g, C_SCORPION)));
        if (consume) drop_shot(g, shot);
        int32_t d = (int32_t)(int16_t)(gm_rd16(g, gm_rd32(g, A5G(g, G_PLAYER)) + 8) - gm_rd16(g, a3 + 8));
        if (d < 0) d = -d;
        d = 0x5DC - lmul(d, 10);
        d = lmul(ldiv32(d, 0x64), 0x64);
        if (d < 0x12C) d = 0x12C;
        add_score(g, lmul((int32_t)gm_rd32(g, A5G(g, G_POINTS)), d));
        gm_wr32(g, A5G(g, G_SCORP_TICK), gm_rd32(g, A5G(g, G_TICKS)) + (uint32_t)(int32_t)(int16_t)rnd(g, 0x168) + 0x168);
        spawn_score_digits(g, a3, (int16_t)(uint16_t)((uint32_t)(uint16_t)d * gm_rd16(g, A5G(g, G_MULT))), 0);
        sprite_deactivate(g, a3);
    }
}

/* ---- timed spawners: psychedelic mushroom (seg3_3A36), multiplier (seg3_3B02), fodder (seg3_3BD8) ---- */

/* picks random cells (27 x 20) until an empty one is found */
static void random_empty_cell(GameCtx *g, int32_t *h, int32_t *v)
{
    for (;;) {
        *h = (int32_t)(int16_t)rnd(g, 0x1B);
        *v = (int32_t)(int16_t)rnd(g, 0x14);
        if (gm_rd32(g, cell32(g, G_GRID, *h, *v)) == 0) return;
    }
}

static void place_on_cell(GameCtx *g, uint32_t s, int32_t h, int32_t v, uint16_t size)
{
    gm_wr16(g, s + 0xA, (uint16_t)(h << 4));
    gm_wr16(g, s + 0x8, (uint16_t)(v << 4));
    gm_wr16(g, s + 0xE, (uint16_t)(size + gm_rd16(g, s + 0xA)));
    gm_wr16(g, s + 0xC, (uint16_t)(size + gm_rd16(g, s + 0x8)));
    gm_wr32(g, s + F_POS_H, shl16(h << 4));
    gm_wr32(g, s + F_POS_V, shl16(v << 4));
}

static void finish_spawn(GameCtx *g, uint32_t s)
{
    gm_wr8(g, s + F_VISIBLE, 1);
    gm_wr8(g, s + F_HIDDEN, 0);
    gm_wr32(g, s + F_SCRATCH, 0);
    gm_wr8(g, s + F_DIRTY, 1);
}

void spawn_psychedelic(GameCtx *g)
{
    uint32_t s = new_sprite(g, G_CLASS_MUSHROOM, TAG_PSYC, -0x640);
    int32_t h, v;
    random_empty_cell(g, &h, &v);
    place_on_cell(g, s, h, v, 0x10);
    gm_wr32(g, s + S_MOVE_CB, g->a5 + 0x1BA);
    gm_wr32(g, s + S_FRAME_CB, g->a5 + 0x1C2);
    gm_wr16(g, s + F_FRAME, 5);
    finish_spawn(g, s);
}

void spawn_multiplier(GameCtx *g)
{
    uint32_t s = new_sprite(g, -0x5F8, TAG_MULT, -0x640);
    int32_t h, v;
    random_empty_cell(g, &h, &v);
    place_on_cell(g, s, h, v, 0xE);
    gm_wr32(g, s + S_MOVE_CB, g->a5 + 0x1CA);
    gm_wr32(g, s + S_FRAME_CB, g->a5 + 0x1D2);
    gm_wr16(g, s + F_FRAME, (uint16_t)(rnd(g, 4) + 1));
    finish_spawn(g, s);
}

void spawn_fodder(GameCtx *g)
{
    int16_t n = 1;
    if (rnd(g, 0x32) == 1) n = (int16_t)(rnd(g, 8) + 2);
    while (n != 0) {
        uint32_t s = new_sprite(g, -0x5F0, TAG_FODR, -0x640);
        int32_t d6 = 0;
        int32_t d5 = (int32_t)(int16_t)rnd(g, 0x17);
        int32_t vx = (int32_t)(int16_t)(rnd(g, 4) + 2) << 16;
        gm_wr32(g, s + F_VEL_H, (uint32_t)vx);
        if (rnd(g, 2) == 1) {
            d6 = (int32_t)(int16_t)(0x1B0 - gm_rd16(g, s + F_W));
            gm_wr32(g, s + F_POS_H, shl16(d6));
            gm_wr32(g, s + F_VEL_H, 0u - gm_rd32(g, s + F_VEL_H));
        } else {
            gm_wr32(g, s + F_POS_H, shl16(d6 << 4));
        }
        gm_wr16(g, s + 0xA, (uint16_t)d6);
        gm_wr16(g, s + 0x8, (uint16_t)(d5 << 4));
        gm_wr16(g, s + 0xE, (uint16_t)(0x14 + gm_rd16(g, s + 0xA)));
        gm_wr16(g, s + 0xC, (uint16_t)(0x14 + gm_rd16(g, s + 0x8)));
        gm_wr32(g, s + F_POS_V, shl16(d5 << 4));
        gm_wr32(g, s + S_MOVE_CB, g->a5 + 0x15A);
        gm_wr32(g, s + S_FRAME_CB, g->a5 + 0x162);
        gm_wr16(g, s + F_FRAME, 1);
        finish_spawn(g, s);
        n--;
    }
}

/* ---- flea spawner (seg3_3CEA) and the extra pede head (seg3_42128) ---- */
void spawn_fleas(GameCtx *g)
{
    int16_t n = 1;
    if (rnd(g, 100) == 1) n = (int16_t)(rnd(g, 10) + 3);
    if ((int16_t)gm_rd16(g, A5G(g, G_LEVEL_SHOWN)) < 11) n = 1;
    while (n != 0) {
        n--;
        int sideways = rnd(g, 15) == 1;
        if ((int16_t)gm_rd16(g, A5G(g, G_LEVEL_SHOWN)) < 6) sideways = 0;
        uint32_t f = new_sprite(g, -0x628, TAG_FLEA, -0x640);
        gm_wr32(g, f + S_MOVE_CB, g->a5 + 0x11A);
        gm_wr32(g, f + S_FRAME_CB, g->a5 + 0x122);
        gm_wr8(g, f + F_VISIBLE, 1);
        gm_wr8(g, f + F_HIDDEN, 0);
        int32_t col = (int32_t)(int16_t)(rnd(g, 0x1B) << 4);
        gm_wr32(g, f + F_POS_H, shl16(col));
        gm_wr32(g, f + F_POS_V, 0);
        gm_wr32(g, f + F_SCRATCH, 0xFFFF);
        gm_wr32(g, f + F_VEL_H, 0);
        gm_wr32(g, f + F_VEL_V, 0x50000u);
        if (rnd(g, 10) == 0) gm_wr32(g, f + F_VEL_V, 0x80000u);
        if (sideways) {
            int32_t vx = (int32_t)(int16_t)(20 + rnd(g, 0x32)) << 12;
            gm_wr32(g, f + F_VEL_H, (uint32_t)vx);
            if (rnd(g, 2) == 0) gm_wr32(g, f + F_VEL_H, 0u - gm_rd32(g, f + F_VEL_H));
            gm_wr32(g, f + F_VEL_V, 0);
        }
        gm_wr16(g, f + 8, (uint16_t)asr16(gm_rd32(g, f + F_POS_V)));
        gm_wr16(g, f + 0xA, (uint16_t)asr16(gm_rd32(g, f + F_POS_H)));
        gm_wr16(g, A5G(g, G_FLEA_COUNT), (uint16_t)(gm_rd16(g, A5G(g, G_FLEA_COUNT)) + 1));
    }
}

/* once a head has touched down, new heads enter from the side every so many ticks */
void add_pede_head(GameCtx *g)
{
    int32_t gap = (int32_t)(gm_rd32(g, A5G(g, G_TICKS)) - gm_rd32(g, A5G(g, G_TOUCH_TICK)));
    if (gap < (int32_t)gm_rd32(g, A5G(g, -0x6D6))) return;
    gm_wr32(g, A5G(g, -0x6D6), gm_rd32(g, A5G(g, -0x6D6)) - 4);
    if (!(30 < (int32_t)gm_rd32(g, A5G(g, -0x6D6)))) gm_wr32(g, A5G(g, -0x6D6), 30);
    gm_wr32(g, A5G(g, G_TOUCH_TICK), gm_rd32(g, A5G(g, G_TICKS)));
    call_sound(g, -0x53C, 3);
    gm_wr16(g, A5G(g, G_PEDE_ALIVE), (uint16_t)(gm_rd16(g, A5G(g, G_PEDE_ALIVE)) + 1));
    int32_t d7 = rnd(g, 2) == 1 ? 0 : 0x1A0, d6 = 0x160;
    uint32_t s = new_sprite(g, -0x634, TAG_PHED, -0x648);
    uint32_t rec = gm_alloc(g, 0x8C);
    gm_wr8(g, rec + R_DIR, d7 == 0 ? 2 : 1);
    gm_wr8(g, rec + R_NEXT, 4);
    gm_wr16(g, rec + R_TARGET, 0);
    gm_wr32(g, rec + R_AHEAD, 0);
    gm_wr16(g, rec + 2, 0);
    gm_wr16(g, rec, 0);
    gm_wr32(g, rec + R_BEHIND, 0);
    gm_wr16(g, rec + 0x88, 1);
    gm_wr8(g, rec + R_DIVE, 0);
    gm_wr32(g, s + F_SCRATCH, rec);
    gm_wr32(g, s + F_POS_H, shl16(d7));
    gm_wr32(g, s + F_POS_V, shl16(d6));
    /* _SetRect(rect, left = d7, top = d6, right = d7 + width, bottom = d6 + height) */
    gm_wr16(g, s + 0xA, (uint16_t)d7);
    gm_wr16(g, s + 0x8, (uint16_t)d6);
    gm_wr16(g, s + 0xE, (uint16_t)(gm_rd16(g, s + F_W) + d7));
    gm_wr16(g, s + 0xC, (uint16_t)(gm_rd16(g, s + F_H) + d6));
    gm_wr32(g, s + S_MOVE_CB, g->a5 + 0xDA);
    gm_wr32(g, s + S_FRAME_CB, g->a5 + 0xE2);
    gm_wr32(g, cell32(g, G_PEDE_GRID, (d7 + 8) >> 4, (d6 + 8) >> 4), s);
}

/* ---- shield, house cleaning and the timed launches of the scorpion, gecko and fleas ---- */
#define TAG_SHEL 0x7368656Cu

static int sound_playing(GameCtx *g, int off)       /* jump-table routine: is this sound still playing? */
{
    uint8_t raw[4]; put32(raw, gm_rd32(g, A5G(g, off)));
    return (uint8_t)ext(g, (int)(g->a5 + 0x3D2), raw, 4) != 0;
}

/* seg3_2048: the sprinkly shield (or restart of its timer if already on) */
void shield_start(GameCtx *g)
{
    gm_wr32(g, A5G(g, -0x672), gm_rd32(g, A5G(g, G_TICKS)));
    if (gm_rd8(g, A5G(g, G_SHIELD))) return;
    gm_wr8(g, A5G(g, G_SHIELD), 1);
    uint32_t s = new_sprite(g, -0x5EC, TAG_SHEL, G_LIST_SHOTS);
    gm_wr32(g, s + S_MOVE_CB, g->a5 + 0x1DA);
    gm_wr32(g, s + S_FRAME_CB, g->a5 + 0x1E2);
    uint32_t pl = gm_rd32(g, A5G(g, G_PLAYER));
    gm_wr32(g, s + F_POS_H, gm_rd32(g, pl + F_POS_H));
    gm_wr32(g, s + F_POS_V, gm_rd32(g, pl + F_POS_V) - 0x50000u);
    gm_wr16(g, s + 8, (uint16_t)asr16(gm_rd32(g, s + F_POS_V)));
    gm_wr16(g, s + 0xA, (uint16_t)asr16(gm_rd32(g, s + F_POS_H)));
}

/* seg3_490E: the shield sits exactly on the shooter */
void move_shield(GameCtx *g, uint32_t s)
{
    uint32_t pl = gm_rd32(g, A5G(g, G_PLAYER));
    gm_wr32(g, s + F_POS_H, gm_rd32(g, pl + F_POS_H));
    gm_wr32(g, s + F_POS_V, gm_rd32(g, pl + F_POS_V));
}

/* seg3_4874: the shield warns after 1620 ticks and ends after 1800 */
void frame_shield(GameCtx *g, uint32_t s)
{
    int16_t f = (int16_t)(gm_rd16(g, s + F_FRAME) + 1);
    gm_wr16(g, s + F_FRAME, (uint16_t)f);
    if (f > (int16_t)gm_rd16(g, s + F_FRAMES)) gm_wr16(g, s + F_FRAME, 1);
    int32_t age = (int32_t)(gm_rd32(g, A5G(g, G_TICKS)) - gm_rd32(g, A5G(g, -0x672)));
    if (age > 0x654 && !gm_rd8(g, A5G(g, G_NOBONUS)) && !sound_playing(g, -0x4FC)) call_sound(g, -0x4FC, 3);
    age = (int32_t)(gm_rd32(g, A5G(g, G_TICKS)) - gm_rd32(g, A5G(g, -0x672)));
    if (age > 0x708 && !gm_rd8(g, A5G(g, G_NOBONUS))) {
        gm_wr8(g, A5G(g, G_SHIELD), 0);
        gm_wr8(g, A5G(g, G_P_66E), 1);
        kill_sprite(g, s);
        call_sound(g, S_LEAVE, 2);
    }
}

/* seg3_20CC: house cleaning: every mushroom below row 18 explodes */
void house_clean(GameCtx *g)
{
    uint32_t m = gm_rd32(g, gm_rd32(g, A5G(g, G_LIST_MUSHROOMS)) + L_FIRST);
    while (m) {
        uint32_t next = gm_rd32(g, m + S_NEXT);
        int32_t h = asr16(gm_rd32(g, m + F_POS_H)) >> 4, v = asr16(gm_rd32(g, m + F_POS_V)) >> 4;
        if ((int16_t)v > 0x12) {
            spawn_explosion(g, m, gm_rd32(g, A5G(g, G_EXPL_VALUE)));
            grid_remove_mushroom(g, (int16_t)h, (int16_t)v);
            call_sound(g, S_SHIP, 3);
        }
        m = next;
    }
}

/* seg3_2334: warns, then launches the scorpion, the gecko and fleas when their timers are due */
void timed_launches(GameCtx *g)
{
    int32_t now = (int32_t)gm_rd32(g, A5G(g, G_TICKS));
    uint32_t sc = gm_rd32(g, A5G(g, -0x590)), ge = gm_rd32(g, A5G(g, -0x594));
    if ((int32_t)(gm_rd32(g, A5G(g, G_SCORP_TICK)) - 30) < now && !gm_rd8(g, sc + F_VISIBLE) && !gm_rd8(g, sc + F_HIDDEN) && !sound_playing(g, -0x54C))
        call_sound(g, -0x54C, 3);
    if (now > (int32_t)gm_rd32(g, A5G(g, G_SCORP_TICK)) && !gm_rd8(g, sc + F_VISIBLE) && !gm_rd8(g, sc + F_HIDDEN)) {
        int16_t lo = (int16_t)gm_rd16(g, A5G(g, G_MUSH_LIMIT));
        int32_t d7 = (int32_t)(int16_t)(((int16_t)(rnd(g, (uint16_t)(30 - lo - 2)) + lo)) << 4);
        int32_t level = (int16_t)gm_rd16(g, A5G(g, G_LEVEL_SHOWN));
        int from_left;
        if (level % 2 == 0) from_left = rnd(g, 4) == 0; else from_left = rnd(g, 4) != 0;
        int32_t d6;
        int poor = (int32_t)gm_rd32(g, A5G(g, G_SCORE)) < 0x1388;
        if (from_left) {
            d6 = 0;
            gm_wr32(g, sc + F_VEL_H, poor ? 0x30000u : 0x40000u);
            gm_wr32(g, sc + F_VEL_V, poor ? 0x30000u : 0x40000u);
        } else {
            d6 = 0x198;
            gm_wr32(g, sc + F_VEL_H, poor ? 0xFFFD0000u : 0xFFFC0000u);
            gm_wr32(g, sc + F_VEL_V, poor ? 0x30000u : 0x40000u);
        }
        gm_wr32(g, sc + F_POS_H, shl16(d6));
        gm_wr32(g, sc + F_POS_V, shl16(d7));
        gm_wr32(g, sc + F_SCRATCH, 0);
        engine_activate(g, sc);
    }
    if (!gm_rd8(g, ge + F_VISIBLE) && !gm_rd8(g, ge + F_HIDDEN) && now > (int32_t)gm_rd32(g, A5G(g, -0x6B6)) && gm_rd32(g, A5G(g, -0x6B6)) != 0) {
        call_sound(g, -0x508, 3);
        gm_wr32(g, A5G(g, -0x6B6), 0);
        int32_t d7 = (int32_t)(int16_t)(rnd(g, 0x11) << 4);
        gm_wr32(g, ge + F_POS_H, shl16(0x186));
        gm_wr32(g, ge + F_POS_V, shl16(d7));
        gm_wr32(g, ge + F_VEL_H, 0x30000u);
        if ((int32_t)gm_rd32(g, A5G(g, G_SCORE)) > 0x4E20) gm_wr32(g, ge + F_VEL_H, 0x50000u);
        if (rnd(g, 4) == 1) gm_wr32(g, ge + F_VEL_H, 0x30000u);
        engine_activate(g, ge);
    }
    if ((int16_t)gm_rd16(g, A5G(g, G_MUSH_COUNT)) < (int16_t)gm_rd16(g, A5G(g, G_FLEA_LIMIT)) && now > (int32_t)gm_rd32(g, A5G(g, G_FLEA_TICK)) &&
        gm_rd16(g, A5G(g, G_FLEA_COUNT)) == 0 && gm_rd16(g, A5G(g, G_MUSH_LEVEL)) != 0) {
        gm_wr16(g, A5G(g, G_FLEA_POINTS), 200);
        call_sound(g, -0x578, 3);
        spawn_fleas(g);
    }
}

/* seg3_30F6: clear the mushroom list and grid, then plant 30 mushrooms in random empty cells */
void clear_playfield(GameCtx *g)
{
    gm_wr16(g, A5G(g, G_MUSH_COUNT), 0);
    uint32_t list = gm_rd32(g, A5G(g, G_LIST_MUSHROOMS));
    uint32_t m = gm_rd32(g, list + L_FIRST);
    while (m) {
        uint32_t next = gm_rd32(g, m + S_NEXT);
        engine_unlink(g, list, m);
        m = next;
    }
    for (int16_t h = 0; h < 0x1B; h++)
        for (int16_t v = 0; v < 0x1E; v++) gm_wr32(g, cell(g, h, v), 0);
    for (int16_t i = 0; i < 0x1E; i++) {
        int16_t h, v;
        do {
            h = (int16_t)rnd(g, 0x1B);
            v = (int16_t)rnd(g, 0x1D);
        } while (gm_rd32(g, cell(g, h, v)) != 0);
        spawn_mushroom(g, h, v);
    }
}

/* ---- building a round (seg3_299A): timers, mushrooms, the Pentipede chain, gecko, scorpion, shooter and shield ---- */
static void set_rect_at(GameCtx *g, uint32_t s, int32_t left, int32_t top)   /* _SetRect(s+8, left, top, left + width, top + height) */
{
    gm_wr16(g, s + 0xA, (uint16_t)left);
    gm_wr16(g, s + 0x8, (uint16_t)top);
    gm_wr16(g, s + 0xE, (uint16_t)(gm_rd16(g, s + F_W) + left));
    gm_wr16(g, s + 0xC, (uint16_t)(gm_rd16(g, s + F_H) + top));
}

static uint32_t new_segment_record(GameCtx *g, uint8_t dir, uint32_t ahead)
{
    uint32_t rec = gm_alloc(g, 0x8C);
    gm_wr8(g, rec + R_DIR, dir);
    gm_wr8(g, rec + R_NEXT, 4);
    gm_wr16(g, rec + R_TARGET, 0);
    gm_wr32(g, rec + R_AHEAD, ahead);
    gm_wr32(g, rec + R_BEHIND, 0);
    gm_wr16(g, rec + 2, 0);
    gm_wr16(g, rec, 0);
    return rec;
}

static void place_segment(GameCtx *g, uint32_t s, int32_t h, int32_t v)
{
    gm_wr32(g, s + F_POS_H, shl16(h));
    gm_wr32(g, s + F_POS_V, shl16(v));
    set_rect_at(g, s, h, v);
    gm_wr32(g, s + S_MOVE_CB, g->a5 + 0xDA);
    gm_wr32(g, s + S_FRAME_CB, g->a5 + 0xE2);
    gm_wr32(g, cell32(g, G_PEDE_GRID, (h + 8) >> 4, (v + 8) >> 4), s);
}

/* a due time 2 to 62 seconds ahead: ticks + 120 + rand(range) * 60 */
static uint32_t time_after(GameCtx *g, uint16_t range)
{
    int32_t r = (int32_t)(int16_t)(rnd(g, range) * 60);
    return gm_rd32(g, A5G(g, G_TICKS)) + 0x78u + (uint32_t)(int32_t)(int16_t)r;
}

static void unlink_all(GameCtx *g, uint32_t list)
{
    uint32_t s = gm_rd32(g, list + L_FIRST);
    while (s) {
        uint32_t next = gm_rd32(g, s + S_NEXT);
        engine_unlink(g, list, s);
        s = next;
    }
}

void build_round(GameCtx *g)
{
    uint32_t ticks = gm_rd32(g, A5G(g, G_TICKS));
    gm_wr8(g, A5G(g, -0x729), 0);
    gm_wr16(g, A5G(g, G_FLEA_COUNT), 0);
    gm_wr16(g, A5G(g, G_SHOTS_IN_FLIGHT), 0);
    gm_wr32(g, A5G(g, G_LAST_SHOT), ticks);
    gm_wr8(g, A5G(g, G_DEAD), 0);
    if (gm_rd8(g, A5G(g, G_PSYC_FLAG)) || gm_rd8(g, A5G(g, G_DISPLAY_FLAG))) {
        uint8_t raw[5]; put32(raw, gm_rd32(g, A5G(g, G_PALETTE_H))); raw[4] = 1;
        ext(g, (int)(g->a5 + 0x36A), raw, 5);
        gm_wr32(g, A5G(g, G_POINTS), 1);
    }
    gm_wr32(g, A5G(g, G_POINTS), 1);
    gm_wr32(g, A5G(g, G_BONUS), 0x7D0);
    gm_wr16(g, A5G(g, G_BONUS_SHOWN), 0);
    gm_wr32(g, A5G(g, G_BONUS_TICK), ticks + 0x14);
    gm_wr8(g, A5G(g, G_P_66E), 1);
    gm_wr8(g, A5G(g, G_NOBONUS), 0);
    gm_wr8(g, A5G(g, G_TOUCH_FLAG), 0);
    if (!(30 < (int32_t)gm_rd32(g, A5G(g, -0x6D6)))) gm_wr32(g, A5G(g, -0x6D6), 0xF0);
    gm_wr32(g, A5G(g, -0x6D6), gm_rd32(g, A5G(g, -0x6D6)) - 3);
    gm_wr32(g, A5G(g, G_TIMER_A), 0);
    if (rnd(g, 10) == 1) gm_wr32(g, A5G(g, G_TIMER_A), time_after(g, 30));
    gm_wr32(g, A5G(g, G_TIMER_B), 0);
    if (rnd(g, 3) == 1) gm_wr32(g, A5G(g, G_TIMER_B), time_after(g, 30));
    gm_wr16(g, A5G(g, G_MULT_SEEN), 0);
    gm_wr32(g, A5G(g, G_TIMER_C), 0);
    if (rnd(g, 3) == 1) gm_wr32(g, A5G(g, G_TIMER_C), time_after(g, 30));
    gm_wr32(g, A5G(g, -0x6B6), 0);
    if (rnd(g, 4) == 1) gm_wr32(g, A5G(g, -0x6B6), time_after(g, 20));
    gm_wr16(g, A5G(g, -0x6E0), 0);
    gm_wr16(g, A5G(g, G_MUSH_LEVEL), gm_rd16(g, A5G(g, G_LEVEL_SHOWN)));
    while ((int16_t)gm_rd16(g, A5G(g, G_MUSH_LEVEL)) > 11) {
        gm_wr16(g, A5G(g, -0x6E0), (uint16_t)(gm_rd16(g, A5G(g, -0x6E0)) + 1));
        gm_wr16(g, A5G(g, G_MUSH_LEVEL), (uint16_t)(gm_rd16(g, A5G(g, G_MUSH_LEVEL)) - 11));
    }
    gm_wr16(g, A5G(g, G_MUSH_LEVEL), (uint16_t)(gm_rd16(g, A5G(g, G_MUSH_LEVEL)) - 1));
    while ((int16_t)gm_rd16(g, A5G(g, -0x6E0)) > 10) gm_wr16(g, A5G(g, -0x6E0), (uint16_t)(gm_rd16(g, A5G(g, -0x6E0)) - 11));
    if ((int16_t)gm_rd16(g, A5G(g, G_MUSH_LEVEL)) < 3) gm_wr32(g, A5G(g, -0x6B6), 0);
    {   /* seg3_00D8: clear the moved and drawn flags of the sprite engine */
        uint32_t e = gm_rd32(g, A5G(g, G_ENGINE));
        gm_wr8(g, e + E_MOVED, 0);
        gm_wr8(g, e + E_DRAWN, 0);
    }
    gm_wr16(g, A5G(g, G_PEDE_ALIVE), 0);
    for (uint32_t m = gm_rd32(g, gm_rd32(g, A5G(g, G_LIST_MUSHROOMS)) + L_FIRST); m;) {
        uint32_t next = gm_rd32(g, m + S_NEXT);
        if ((int16_t)gm_rd16(g, m + F_FRAME) > 4) gm_wr16(g, m + F_FRAME, (uint16_t)((gm_rd16(g, A5G(g, G_MUSH_LEVEL)) << 2) + 5));
        gm_wr8(g, m + F_DIRTY, 1);
        gm_wr8(g, m + F_HIDDEN, 1);
        m = next;
    }
    for (int32_t h = 0; 0x1B > h; h++)
        for (int32_t v = 0; 0x1E > v; v++) gm_wr32(g, cell32(g, G_PEDE_GRID, h, v), 0);
    gm_wr16(g, A5G(g, G_STEP), (gm_rd16(g, A5G(g, G_LEVEL_SHOWN)) % 2 != 0) ? 5 : 3);
    if ((int32_t)gm_rd32(g, A5G(g, G_SCORE)) > 0x9C40) gm_wr16(g, A5G(g, G_STEP), 5);
    call0(g, 0x432F8);

    uint32_t plist = gm_rd32(g, A5G(g, G_PEDE_LIST));
    for (uint32_t s = gm_rd32(g, plist + L_FIRST); s;) {
        uint32_t next = gm_rd32(g, s + S_NEXT);
        dispose_ptr(g, gm_rd32(g, s + F_SCRATCH));
        engine_unlink(g, plist, s);
        s = next;
    }
    unlink_all(g, gm_rd32(g, A5G(g, G_LIST_BAD)));
    uint32_t gecko = new_sprite(g, -0x624, TAG_GECK, -0x640);
    gm_wr32(g, A5G(g, -0x594), gecko);
    gm_wr32(g, gecko + F_POS_H, 0); gm_wr32(g, gecko + F_POS_V, 0);
    gm_wr32(g, gecko + S_MOVE_CB, g->a5 + 0x12A); gm_wr32(g, gecko + S_FRAME_CB, g->a5 + 0x132);
    gm_wr8(g, gecko + F_VISIBLE, 0); gm_wr8(g, gecko + F_HIDDEN, 0);
    uint32_t scorp = new_sprite(g, -0x620, TAG_SCOR, -0x640);
    gm_wr32(g, A5G(g, -0x590), scorp);
    gm_wr32(g, scorp + F_POS_H, 0); gm_wr32(g, scorp + F_POS_V, 0);
    gm_wr32(g, scorp + S_MOVE_CB, g->a5 + 0x13A); gm_wr32(g, scorp + S_FRAME_CB, g->a5 + 0x142);
    gm_wr8(g, scorp + F_VISIBLE, 0); gm_wr8(g, scorp + F_HIDDEN, 0);
    gm_wr16(g, scorp + S_FRAME_INTERVAL, 2);

    int32_t k = (int32_t)(int16_t)(gm_rd16(g, A5G(g, G_LEVEL_SHOWN)) - 1) % 11;
    int16_t chain = (int16_t)(11 - k);
    uint32_t prev = 0;
    int32_t h = (int32_t)(int16_t)((rnd(g, 3) + 0xE) << 4), v = 0;
    int16_t frame = 1;
    for (int16_t i = 1; i <= chain; i++) {
        gm_wr16(g, A5G(g, G_PEDE_ALIVE), (uint16_t)(gm_rd16(g, A5G(g, G_PEDE_ALIVE)) + 1));
        uint32_t s = new_sprite(g, -0x634, 0x70626F64u, -0x648);
        gm_wr16(g, s + F_FRAME, (uint16_t)frame);
        frame++;
        if (frame > (int16_t)gm_rd16(g, s + F_FRAMES)) frame = 1;
        uint32_t rec = new_segment_record(g, 2, prev);
        gm_wr16(g, rec + 0x88, (uint16_t)(rnd(g, 0xE) + 1));
        gm_wr8(g, rec + R_DIVE, 0);
        int16_t step = (int16_t)gm_rd16(g, A5G(g, G_STEP));
        for (int16_t j = 0; (int16_t)(16 / step) > j; j++) {
            uint32_t at = (uint32_t)((int32_t)(int16_t)gm_rd16(g, rec + 2) * 12);
            gm_wr32(g, rec + 8 + at, (uint32_t)v);
            gm_wr32(g, rec + 4 + at, (uint32_t)h);
            gm_wr16(g, rec + 2, (uint16_t)(gm_rd16(g, rec + 2) + 1));
            if ((int16_t)gm_rd16(g, rec + 2) >= 10) gm_wr16(g, rec + 2, 0);
        }
        gm_wr32(g, s + F_SCRATCH, rec);
        if (i == 1) { gm_wr32(g, s, TAG_PHED); gm_wr8(g, rec + R_DIVE, 0); }
        place_segment(g, s, h, v);
        if (prev) gm_wr32(g, gm_rd32(g, prev + F_SCRATCH) + R_BEHIND, s);
        prev = s;
    }
    for (int16_t i = 1; k >= i; i++) {
        gm_wr16(g, A5G(g, G_PEDE_ALIVE), (uint16_t)(gm_rd16(g, A5G(g, G_PEDE_ALIVE)) + 1));
        do {
            h = (int32_t)(int16_t)(rnd(g, 0x1B) << 4);
            v = 0;
        } while (gm_rd32(g, cell32(g, G_PEDE_GRID, (h + 8) >> 4, (v + 8) >> 4)) != 0);
        uint32_t s = new_sprite(g, -0x634, TAG_PHED, -0x648);
        uint32_t rec = new_segment_record(g, rnd(g, 2) ? 2 : 1, 0);
        gm_wr16(g, rec + 0x88, 1);
        gm_wr8(g, rec + R_DIVE, 0);
        gm_wr32(g, s + F_SCRATCH, rec);
        place_segment(g, s, h, v);
    }

    unlink_all(g, gm_rd32(g, A5G(g, G_LIST_SHOTS)));
    uint32_t pl = new_sprite(g, -0x630, 0x73687472u, -0x644);
    gm_wr32(g, A5G(g, G_PLAYER), pl);
    int32_t px = (int32_t)(int16_t)((int16_t)(gm_rd16(g, A5G(g, -0x742)) - gm_rd16(g, A5G(g, -0x746))) >> 1);
    int32_t py = (int32_t)(int16_t)(gm_rd16(g, A5G(g, -0x744)) - gm_rd16(g, A5G(g, -0x748)) - 0x10);
    gm_wr32(g, pl + F_POS_H, shl16(px));
    gm_wr32(g, pl + F_POS_V, shl16(py));
    gm_wr32(g, pl + S_MOVE_CB, g->a5 + 0xEA);
    gm_wr32(g, pl + S_FRAME_CB, g->a5 + 0xF2);
    if (gm_rd8(g, A5G(g, G_SHIELD))) {
        uint32_t sh = new_sprite(g, -0x5EC, TAG_SHEL, G_LIST_SHOTS);
        gm_wr32(g, sh + S_MOVE_CB, g->a5 + 0x1DA);
        gm_wr32(g, sh + S_FRAME_CB, g->a5 + 0x1E2);
        gm_wr32(g, sh + F_POS_H, gm_rd32(g, pl + F_POS_H));
        gm_wr32(g, sh + F_POS_V, gm_rd32(g, pl + F_POS_V) - 0x50000u);
        gm_wr16(g, sh + 8, (uint16_t)asr16(gm_rd32(g, sh + F_POS_V)));
        gm_wr16(g, sh + 0xA, (uint16_t)asr16(gm_rd32(g, sh + F_POS_H)));
    }
    gm_wr16(g, A5G(g, G_PENDING_Y), 0);
    gm_wr16(g, A5G(g, G_PENDING_X), 0);
    gm_wr32(g, A5G(g, G_FLEA_TICK), gm_rd32(g, A5G(g, G_TICKS)) + 0x78);
    gm_wr32(g, A5G(g, G_SCORP_TICK), gm_rd32(g, A5G(g, G_TICKS)) + 0xF0);
    if (gm_rd8(g, A5G(g, G_MUSIC))) {
        uint8_t raw[12]; put32(raw, gm_rd32(g, A5G(g, -0x58C))); put32(raw + 4, 0x50004u); put32(raw + 8, g->a5 + 0x92);
        ext(g, (int)(g->a5 + 0x3E2), raw, 12);
    }
    {
        uint8_t raw[6]; put16(raw, 0x1E); put32(raw + 2, gm_rd32(g, A5G(g, -0x728)));
        ext(g, (int)(g->a5 + 0x39A), raw, 6);
    }
    gm_wr8(g, A5G(g, G_PSYC_FLAG), 0);
    gm_wr16(g, A5G(g, -0x718), 0);
}

/* ---- the in-game loop: dirty marking under moved sprites (seg3_0F2C) and one pass of the loop (seg3_123C) ---- */

/* marks the mushroom in the grid cell containing the point, and the sprite, as needing a redraw */
static void touch_cell(GameCtx *g, uint32_t s, int16_t px, int16_t py)
{
    int16_t h = (int16_t)(px >> 4), v = (int16_t)(py >> 4);
    if (h < 0 || v < 0 || h >= 0x1B || v >= 0x1E) return;
    uint32_t m = gm_rd32(g, cell(g, h, v));
    if (!m) return;
    gm_wr8(g, m + F_DIRTY, 1);
    gm_wr8(g, s + F_DIRTY, 1);
}

/* the cell under a point given as two coordinates halved after reduction to cells (the centre of a rectangle) */
static void touch_centre(GameCtx *g, uint32_t s, int16_t x0, int16_t x1, int16_t y0, int16_t y1)
{
    int16_t h = (int16_t)(((int16_t)(x0 >> 4) + (int16_t)(x1 >> 4)) >> 1);
    int16_t v = (int16_t)(((int16_t)(y0 >> 4) + (int16_t)(y1 >> 4)) >> 1);
    if (h < 0 || v < 0 || h >= 0x1B || v >= 0x1E) return;
    uint32_t m = gm_rd32(g, cell(g, h, v));
    if (!m) return;
    gm_wr8(g, m + F_DIRTY, 1);
    gm_wr8(g, s + F_DIRTY, 1);
}

void mark_mushrooms_under(GameCtx *g, uint32_t list)                        /* seg3_0F2C */
{
    for (uint32_t s = gm_rd32(g, list + L_FIRST); s; s = gm_rd32(g, s + S_NEXT)) {
        if (!gm_rd8(g, s + F_VISIBLE) && !gm_rd8(g, s + F_HIDDEN)) continue;
        int16_t ol = (int16_t)gm_rd16(g, s + 0x12), ot = (int16_t)gm_rd16(g, s + 0x10), orr = (int16_t)gm_rd16(g, s + 0x16), ob = (int16_t)gm_rd16(g, s + 0x14);
        touch_cell(g, s, ol, ot);
        touch_cell(g, s, orr, ot);
        touch_cell(g, s, ol, ob);
        touch_cell(g, s, orr, ob);
        touch_centre(g, s, ol, orr, ot, ob);
        int16_t nl = (int16_t)gm_rd16(g, s + 0xA), nt = (int16_t)gm_rd16(g, s + 8), nr = (int16_t)gm_rd16(g, s + 0xE), nb = (int16_t)gm_rd16(g, s + 0xC);
        touch_cell(g, s, nl, nt);
        touch_cell(g, s, nr, nt);
        touch_cell(g, s, nl, nb);
        touch_cell(g, s, nr, nb);
        touch_centre(g, s, nl, nr, nt, nb);
    }
}

/* one pass of the loop: returns nonzero while the loop should go on (the original keeps going while the "playing" and "running" flags stay set) */
/* ---- the end-of-round routines (seg3_26C2, seg3_2836): loops of their own that wait on Ticks while the screen keeps running ---- */

static int round_wait(GameCtx *g)
{
    if (g->wait) g->wait(g->ud); else gm_wr32(g, 0x16A, gm_rd32(g, 0x16A) + 1);
    return !g->quit;
}

static int sound_busy(GameCtx *g, uint32_t id)
{
    uint8_t raw[4]; put32(raw, id);
    return (ext(g, (int)(g->a5 + 0x3D2u), raw, 4) & 0xFFu) != 0;
}

/* what the loops run every pass: (the frame counter), the score display, the draw pass, the four lists' mushroom marks, the move pass */
static void round_frame(GameCtx *g, int count_tick)
{
    if (count_tick) {
        uint32_t now = gm_rd32(g, 0x16A);
        if (now != gm_rd32(g, A5G(g, -0x6DA))) {
            gm_wr32(g, A5G(g, G_TICKS), gm_rd32(g, A5G(g, G_TICKS)) + 1);
            gm_wr32(g, A5G(g, -0x6DA), now);
        }
    }
    update_score_display(g);
    engine_draw_pass(g);
    mark_mushrooms_under(g, gm_rd32(g, A5G(g, -0x644)));
    mark_mushrooms_under(g, gm_rd32(g, A5G(g, -0x648)));
    mark_mushrooms_under(g, gm_rd32(g, A5G(g, -0x640)));
    mark_mushrooms_under(g, gm_rd32(g, A5G(g, -0x63C)));
    engine_move_pass(g);
}

void end_round_repair(GameCtx *g, uint16_t which)
{
    if (gm_rd8(g, A5G(g, G_MUSIC))) { uint8_t w[2]; put16(w, 4); ext(g, (int)(g->a5 + 0x3DAu), w, 2); }
    sound(g, which, 3);                                             /* the snd resource with the id passed in: 0x90 level clear, 0x91 the shooter is back */
    while (sound_busy(g, which)) {
        round_frame(g, 0);
        if (!round_wait(g)) return;
    }
    uint32_t m = gm_rd32(g, gm_rd32(g, A5G(g, G_LIST_MUSHROOMS)) + 4);
    while (m) {
        uint32_t t0 = gm_rd32(g, 0x16A), next = gm_rd32(g, m + 0x5A);
        int16_t fresh = (int16_t)(gm_rd16(g, A5G(g, G_MUSH_LEVEL)) * 4 + 5), frame = (int16_t)gm_rd16(g, m + F_FRAME);
        int32_t span = 0;
        if (fresh != frame) {
            gm_wr16(g, m + F_FRAME, (frame < 5 && which == 0x90) ? 1 : (uint16_t)fresh);
            spawn_fragments(g, (uint32_t)asr16(gm_rd32(g, m + F_POS_H)), (uint32_t)asr16(gm_rd32(g, m + F_POS_V)));
            call_sound(g, -0x55C, 3);
            gm_wr32(g, A5G(g, G_SCORE), gm_rd32(g, A5G(g, G_SCORE)) + (uint32_t)lmul((int32_t)gm_rd32(g, A5G(g, G_POINTS)), 5));
            spawn_score_digits(g, m, (int16_t)(5u * gm_rd16(g, A5G(g, G_MULT))), 0);
            gm_wr8(g, m + 0x1D, 1);
            gm_wr8(g, m + 0x1E, 1);
            span = 7;
        }
        while ((int32_t)(gm_rd32(g, 0x16A) - t0) < span) {
            round_frame(g, 1);
            if (!round_wait(g)) return;
        }
        m = next;
    }
}

void end_round_bonus(GameCtx *g)
{
    uint32_t t = gm_rd32(g, A5G(g, G_TICKS));
    int state = 1, run = 1;
    int32_t mult = 1;
    while (run) {
        uint32_t now = gm_rd32(g, 0x16A);
        if (now != gm_rd32(g, A5G(g, -0x6DA))) {
            gm_wr32(g, A5G(g, G_TICKS), gm_rd32(g, A5G(g, G_TICKS)) + 1);
            gm_wr32(g, A5G(g, -0x6DA), now);
        }
        switch (state) {
        case 1:
            if (gm_rd16(g, A5G(g, G_MULT_SEEN))) {
                mult = (int16_t)gm_rd16(g, A5G(g, G_MULT_SEEN));
                call_sound(g, -0x534, 3);
                gm_wr16(g, A5G(g, G_MULT_SEEN), 0);
                gm_wr8(g, A5G(g, G_REDRAW), 1);
            }
            state++;
            break;
        case 2:
            if (!sound_busy(g, gm_rd32(g, A5G(g, -0x534)))) {
                state++;
                if (mult != 1) {
                    call_sound(g, -0x540, 3);
                    gm_wr32(g, A5G(g, G_BONUS), (uint32_t)lmul((int32_t)gm_rd32(g, A5G(g, G_BONUS)), mult));
                }
            }
            break;
        case 3:
            if (!sound_busy(g, gm_rd32(g, A5G(g, -0x540)))) state++;
            break;
        case 4:
            if ((int32_t)(gm_rd32(g, A5G(g, G_TICKS)) - t) - 4 > 0) {
                int32_t b = (int32_t)gm_rd32(g, A5G(g, G_BONUS)) - 0x5A;
                gm_wr32(g, A5G(g, G_BONUS), b < 0 ? 0u : (uint32_t)b);
                gm_wr32(g, A5G(g, G_SCORE), gm_rd32(g, A5G(g, G_SCORE)) + 0x5A);
                t = gm_rd32(g, A5G(g, G_TICKS));
                call_sound(g, -0x520, 2);
            }
            if ((int32_t)gm_rd32(g, A5G(g, G_BONUS)) <= 0) { call_sound(g, -0x52C, 3); state++; }
            break;
        case 5:
            if (!sound_busy(g, gm_rd32(g, A5G(g, -0x52C)))) state++;
            break;
        case 6:
            run = 0;
            break;
        default:
            break;
        }
        round_frame(g, 0);
        if (!round_wait(g)) return;
    }
}


/* seg4_0378 with the mode byte at A5-0x45F equal to 3 (paused): the typed character is flipped through ROT13 (letters only, case kept) and pushed into a
 * nine-character history (A5-0x46C, newest first); each of the 11 stored code words (ROT13-encoded Pascal strings) is compared with the last typed
 * characters. Typing the word in capitals is what matches, and Caps Lock is the pause key, so the letters come out capital. A match sets the cheat flags,
 * plays a sound and does what its number says (the switch below is the original's, case for case); it does not stop looking at the other words. */
void cheat_key(GameCtx *g, uint8_t ch, const uint8_t codes[11][32])
{
    const int HIST = -0x46C;
    uint8_t f = ch;
    if (ch >= 'A' && ch <= 'Z') { f = (uint8_t)(ch - 13); if (f < 'A') f = (uint8_t)(ch + 13); }
    else if (ch > '`' && ch < '{') { f = (uint8_t)(ch - 13); if (f < 'a') f = (uint8_t)(ch + 13); }
    for (int i = 8; i >= 0; i--) gm_wr8(g, A5G(g, HIST + 1 + i), gm_rd8(g, A5G(g, HIST + i)));
    gm_wr8(g, A5G(g, HIST), f);
    for (int k = 0; k <= 10; k++) {
        unsigned len = codes[k][0];
        int match = len > 0;
        for (unsigned j = 0; j < len && match; j++)
            if (codes[k][len - j] != gm_rd8(g, A5G(g, HIST + (int)j))) match = 0;
        if (!match) continue;
        gm_wr8(g, A5G(g, -0x72A), 1);                         /* cheated: no high score, no contest */
        gm_wr8(g, A5G(g, -0x462), 1);
        sound(g, gm_rd32(g, A5G(g, -0x52C)), 2);
        switch (k) {
        case 0: gm_wr16(g, A5G(g, G_LIVES), (uint16_t)(gm_rd16(g, A5G(g, G_LIVES)) + 5)); break;                                        /* HECTOR */
        case 1: gm_wr8(g, A5G(g, G_MACHINE_GUN), 1); gm_wr32(g, A5G(g, G_SPEED), 14); gm_wr16(g, A5G(g, G_SPEED2), 10); gm_wr8(g, A5G(g, G_REDRAW), 1); break;   /* SQUISH */
        case 2: gm_wr8(g, A5G(g, G_REDRAW), 1); shield_start(g); break;                                                                    /* NALA */
        case 3: gm_wr16(g, A5G(g, G_LEVEL), (uint16_t)(gm_rd16(g, A5G(g, G_LEVEL)) + 11)); gm_wr16(g, A5G(g, G_PEDE_ALIVE), 0); break;   /* MORRISON */
        case 4: gm_wr8(g, A5G(g, G_REDRAW), 1); gm_wr8(g, A5G(g, G_P_66D), 1); break;                                                      /* NICE BOX */
        case 5: gm_wr8(g, A5G(g, G_REDRAW), 1); gm_wr8(g, A5G(g, G_P_66C), 1); break;                                                      /* DJARUM */
        case 6: gm_wr32(g, A5G(g, G_BONUS), gm_rd32(g, A5G(g, G_BONUS)) + 5000); break;                                                    /* USMC */
        case 7: house_clean(g); break;                                                                                                     /* NUKE EM */
        case 8: gm_wr32(g, A5G(g, G_SCORE), gm_rd32(g, A5G(g, G_SCORE)) + 50000); break;                                                   /* PERNTS */
        case 9: gm_wr8(g, A5G(g, -0x72A), 0); break;                                                                                       /* SCUMDOG */
        case 10: gm_wr8(g, A5G(g, G_P_669), 1); break;                                                                                     /* SNAPPLE */
        }
    }
}

/* seg3_2596: the pause. The key is Caps Lock (seg3_3348 tests bit 1 of the byte at 0x17B); the game waits for it to go up, then redraws everything */
void game_pause(GameCtx *g)
{
    gm_wr8(g, A5G(g, -0x45F), 3);                                  /* the mode byte: paused (typed letters are checked against the code words) */
    { uint8_t one[4]; put32(one, 1); ext(g, 0x4085C, one, 4); }     /* seg4_085C: the PAUSED picture */
    if (gm_rd8(g, A5G(g, G_MUSIC))) { uint8_t w[2]; put16(w, 4); ext(g, (int)(g->a5 + 0x3DAu), w, 2); }
    sound(g, 0x8B, 3);
    while ((uint8_t)ext(g, 0x43348, 0, 0)) if (!round_wait(g)) return;
    while (sound_busy(g, 0x8B)) if (!round_wait(g)) return;
    { uint8_t zero[4]; put32(zero, 0); ext(g, 0x4085C, zero, 4); }
    engine_full_redraw(g);
    if (gm_rd8(g, A5G(g, G_MUSIC))) {
        uint8_t raw[12]; put32(raw, gm_rd32(g, A5G(g, -0x58C))); put32(raw + 4, 0x50004u); put32(raw + 8, g->a5 + 0x92);
        ext(g, (int)(g->a5 + 0x3E2u), raw, 12);
    }
    gm_wr16(g, A5G(g, -0x41A), 0);                                  /* the mouse movement gathered meanwhile is dropped */
    gm_wr16(g, A5G(g, -0x41C), 0);
    gm_wr16(g, A5G(g, G_LIVES_SHOWN), (uint16_t)(gm_rd16(g, A5G(g, G_LIVES)) - 1));        /* shown values off by one: the panel is drawn again */
    gm_wr32(g, A5G(g, G_SCORE_SHOWN), gm_rd32(g, A5G(g, G_SCORE)) - 1);
    gm_wr16(g, A5G(g, G_LEVEL_SHOWN2), (uint16_t)(gm_rd16(g, A5G(g, G_LEVEL_SHOWN)) - 1));
    gm_wr8(g, A5G(g, G_REDRAW), 1);
    gm_wr8(g, A5G(g, -0x45F), 2);
}

int game_pass(GameCtx *g)
{
    uint32_t now = gm_rd32(g, 0x16A);
    if (now != gm_rd32(g, A5G(g, -0x6DA))) {
        gm_wr32(g, A5G(g, G_TICKS), gm_rd32(g, A5G(g, G_TICKS)) + 1);
        gm_wr32(g, A5G(g, -0x6DA), gm_rd32(g, 0x16A));
    }
    if (gm_rd8(g, 0x17A) & 0x20) gm_wr8(g, A5G(g, G_RESET), 0);            /* seg3_1666: the key that stops the game */
    engine_move_pass(g);
    mark_mushrooms_under(g, gm_rd32(g, A5G(g, G_LIST_SHOTS)));
    mark_mushrooms_under(g, gm_rd32(g, A5G(g, G_PEDE_LIST)));
    mark_mushrooms_under(g, gm_rd32(g, A5G(g, G_LIST_BAD)));
    mark_mushrooms_under(g, gm_rd32(g, A5G(g, G_LIST_EFFECTS)));
    int32_t ticks = (int32_t)gm_rd32(g, A5G(g, G_TICKS));
    if (ticks - (int32_t)gm_rd32(g, A5G(g, -0x692)) - 1 > 0) {
        if (gm_rd8(g, A5G(g, G_PSYC_FLAG)) && ticks - (int32_t)gm_rd32(g, A5G(g, G_PSYC_TICK_B)) - 5 > 0) {
            gm_wr32(g, A5G(g, G_PSYC_TICK_B), (uint32_t)ticks);
            gm_wr16(g, A5G(g, -0x718), (uint16_t)(gm_rd16(g, A5G(g, -0x718)) + 1));
            if ((int16_t)gm_rd16(g, A5G(g, -0x718)) > 2) {
                gm_wr16(g, A5G(g, -0x718), 0);
                if (ticks - (int32_t)gm_rd32(g, A5G(g, G_PSYC_TICK_A)) > 0x384) {
                    if (gm_rd8(g, A5G(g, G_MUSIC))) {
                        uint8_t raw[12]; put32(raw, gm_rd32(g, A5G(g, -0x58C))); put32(raw + 4, 0x50004u); put32(raw + 8, g->a5 + 0x92);
                        ext(g, (int)(g->a5 + 0x3E2), raw, 12);
                    }
                    gm_wr8(g, A5G(g, G_PSYC_FLAG), 0);
                    gm_wr32(g, A5G(g, G_POINTS), 1);
                    gm_wr16(g, A5G(g, -0x718), 2);
                }
            }
            uint32_t handle = gm_rd32(g, g->a5 + (uint32_t)((int32_t)(int16_t)gm_rd16(g, A5G(g, -0x718)) * 4) - 0x728u);
            uint32_t p = gm_rd32(g, handle);
            uint8_t raw[8]; put32(raw, p + 8); put16(raw + 4, gm_rd16(g, p + 6)); put16(raw + 6, 0);
            ext(g, (int)TRAP_SETENTRIES, raw, 8);
        }
        step_update(g);
        gm_wr32(g, A5G(g, -0x692), gm_rd32(g, A5G(g, G_TICKS)));
    }
    if (gm_rd8(g, A5G(g, -0x729)) && ((int16_t)gm_rd16(g, A5G(g, G_SHOTS_IN_FLIGHT)) > 0 || gm_rd8(g, A5G(g, G_SHIELD)))) {
        for (uint32_t s = gm_rd32(g, gm_rd32(g, A5G(g, G_LIST_SHOTS)) + L_FIRST); s; s = gm_rd32(g, s + S_NEXT)) {
            if (gm_rd32(g, s) == TAG_SHOT && gm_rd8(g, s + F_VISIBLE)) shot_hit(g, s, 1);
            if (gm_rd32(g, s) == TAG_SHEL && gm_rd8(g, s + F_VISIBLE)) {
                shot_hit(g, s, 0);
                engine_activate(g, s);
                gm_wr8(g, s + F_REMOVE, 0);
            }
        }
        gm_wr8(g, A5G(g, -0x729), 0);
    }
    engine_draw_pass(g);
    {   /* _A855 with the port and a rectangle: logged */
        uint8_t raw[8]; put32(raw, gm_rd32(g, A5G(g, -0x738))); put32(raw + 4, g->a5 - 0x740u);
        ext(g, 0xA855, raw, 8);
    }
    if ((int16_t)gm_rd16(g, A5G(g, G_PEDE_ALIVE)) <= 0) {
        /* the wave is over: next wave, the end-of-round sequence, then a new round with the shooter kept in place */
        gm_wr16(g, A5G(g, G_LEVEL_SHOWN), (uint16_t)(gm_rd16(g, A5G(g, G_LEVEL_SHOWN)) + 1));
        gm_wr8(g, A5G(g, G_NOBONUS), 1);
        uint32_t t0 = gm_rd32(g, A5G(g, G_TICKS));
        if (g->native_endwave) { end_round_repair(g, 0x90); end_round_bonus(g); }
        else { uint8_t w[2]; put16(w, 0x90); ext(g, 0x426C2, w, 2); call0(g, 0x42836); }
        if (gm_rd8(g, A5G(g, G_SHIELD))) gm_wr32(g, A5G(g, -0x672), gm_rd32(g, A5G(g, -0x672)) + (gm_rd32(g, A5G(g, G_TICKS)) - t0));
        gm_wr8(g, A5G(g, G_NOBONUS), 0);
        uint32_t pl = gm_rd32(g, A5G(g, G_PLAYER));
        uint32_t keep_h = gm_rd32(g, pl + F_POS_H), keep_v = gm_rd32(g, pl + F_POS_V);
        build_round(g);
        pl = gm_rd32(g, A5G(g, G_PLAYER));
        gm_wr32(g, pl + F_POS_H, keep_h);
        gm_wr32(g, pl + F_POS_V, keep_v);
        engine_move_pass(g);
        call0(g, 0x40098);
        call0(g, 0x405CE);
        call0(g, 0x400C2);
    }
    return gm_rd8(g, A5G(g, G_RESET)) && gm_rd8(g, A5G(g, -0x4D2));
}

/* seg3_05CE: redraw the whole playfield: the saved background goes into the work buffer, every visible sprite is drawn on it, and the work buffer goes to the screen.
 * The two whole-area copies (CopyBits in the original) are reported as host calls 0xC0B1 (saved -> work) and 0xC0B2 (work -> screen). Not covered by the Musashi tests (the original
 * draws through Toolbox calls). */
void engine_full_redraw(GameCtx *g)
{
    if (!gm_rd8(g, A5G(g, G_ENGINE_READY))) return;
    uint32_t e = gm_rd32(g, A5G(g, G_ENGINE));
    ext(g, 0xC0B1, 0, 0);
    uint32_t work_rows = gm_rd32(g, e + E_ROWS_WORK);
    uint32_t d3 = gm_rd16(g, e + E_W26);
    for (uint32_t l = gm_rd32(g, e + E_FIRST_LIST); l; l = gm_rd32(g, l + L_NEXT))
        for (uint32_t s = gm_rd32(g, l + L_FIRST); s; s = gm_rd32(g, s + S_NEXT)) {
            if (!gm_rd8(g, s + F_VISIBLE)) continue;
            blit2(g, frame_code(g, s, F_FRAME, 0xE), row_ptr(g, work_rows, (int16_t)gm_rd16(g, s + 8), (int16_t)gm_rd16(g, s + 0xA)), d3);
        }
    ext(g, 0xC0B2, 0, 0);
}

/* test (APEIRON_PEDEWATCH=1): a head that comes down 5 or more rows within 40 passes without poison is reported with the last 40 passes of its own and its nearest head's position and direction */
int g_watch_reset;
static FILE *g_pede_log;
int pede_log_open(const char *path) { g_pede_log = fopen(path, "w"); return g_pede_log != NULL; }
int pede_log_on(void) { return g_pede_log != NULL; }
void watch_pedes(GameCtx *g, uint32_t pass)
{
    enum { NW = 64, HL = 40 };
    static struct { uint32_t s; int v0; uint32_t t0; int16_t x[HL], v[HL]; int8_t d[HL], n[HL]; uint8_t alive; } w[NW];
    static uint32_t reported;
    FILE *out = g_pede_log ? g_pede_log : stderr;
    uint32_t list = gm_rd32(g, A5G(g, G_PEDE_LIST));
    if (g_watch_reset) { for (int i = 0; i < NW; i++) w[i].s = 0; g_watch_reset = 0; }
    for (int i = 0; i < NW; i++) w[i].alive = 0;
    static int mx = -2, my, mlast = -99; static uint32_t msprite;
    if (mx == -2) { mx = -1; const char *e = getenv("APEIRON_WATCH_MUSH"); if (e) sscanf(e, "%d,%d", &mx, &my); }
    if (mx >= 0) {                  /* test: follow the frame of the mushroom at (x, y): created, hit, restored, gone */
        uint32_t found = 0, ml = gm_rd32(g, A5G(g, G_LIST_MUSHROOMS));
        for (uint32_t m = gm_rd32(g, ml + L_FIRST); m; m = gm_rd32(g, m + S_NEXT))
            if (gm_rd32(g, m) == TAG_SHRM && asr16(gm_rd32(g, m + F_POS_H)) == mx && asr16(gm_rd32(g, m + F_POS_V)) == my) { found = m; break; }
        int f = found ? (int)(int16_t)gm_rd16(g, found + F_FRAME) : -1;
        if (f != mlast || found != msprite) { fprintf(stderr, "MUSH pass %u: %s sprite %06X frame %d (level word %d)%c", pass, found ? "there" : "gone", (unsigned)found, f, (int)(int16_t)gm_rd16(g, A5G(g, G_STEP)), 10); mlast = f; msprite = found; }
    }
    static int dump_lo = -2, dump_hi;
    if (dump_lo == -2) { dump_lo = -1; const char *e = getenv("APEIRON_WATCH_DUMP"); if (e) sscanf(e, "%d,%d", &dump_lo, &dump_hi); }
    if (dump_lo >= 0 && pass >= (uint32_t)dump_lo && pass <= (uint32_t)dump_hi) {          /* test: every head, every pass, in a window */
        g_dump_on = 1;
        fprintf(out, "P%u", pass);
        for (uint32_t s = gm_rd32(g, list + L_FIRST); s; s = gm_rd32(g, s + S_NEXT))
            if (gm_rd32(g, s) == TAG_PHED) { uint32_t rec = gm_rd32(g, s + F_SCRATCH); fprintf(out, " %04X:%d,%d,d%d,n%d,v%d", (unsigned)(s & 0xFFFF), asr16(gm_rd32(g, s + F_POS_H)), asr16(gm_rd32(g, s + F_POS_V)), (int)gm_rd8(g, rec + R_DIR), (int)gm_rd8(g, rec + R_NEXT), (int)gm_rd8(g, rec + R_DIVE)); }
        fputc(10, out);
    }
    for (uint32_t s = gm_rd32(g, list + L_FIRST); s; s = gm_rd32(g, s + S_NEXT)) {
        if (gm_rd32(g, s) != TAG_PHED) continue;
        uint32_t rec = gm_rd32(g, s + F_SCRATCH);
        int k = -1;
        for (int i = 0; i < NW; i++) if (w[i].s == s) { k = i; break; }
        if (k < 0) { for (int i = 0; i < NW; i++) if (!w[i].s) { k = i; break; } if (k < 0) continue; w[k].s = s; w[k].t0 = pass; w[k].v0 = asr16(gm_rd32(g, s + F_POS_V)); }
        w[k].alive = 1;
        int h = (int)(pass % HL);
        w[k].x[h] = (int16_t)asr16(gm_rd32(g, s + F_POS_H)); w[k].v[h] = (int16_t)asr16(gm_rd32(g, s + F_POS_V));
        w[k].d[h] = (int8_t)gm_rd8(g, rec + R_DIR); w[k].n[h] = (int8_t)gm_rd8(g, rec + R_NEXT);
        int v = w[k].v[h];
        if (pass - w[k].t0 > HL || v < w[k].v0) { w[k].v0 = v; w[k].t0 = pass; continue; }
        if (v - w[k].v0 >= 80 && !gm_rd8(g, rec + R_DIVE) && pass - reported > 120 && pass - w[k].t0 >= 10) {
            reported = pass;
            int best = -1, bd = 1 << 30;
            for (int j = 0; j < NW; j++) if (j != k && w[j].s && w[j].alive) { int dx = w[j].x[h] - w[k].x[h], dy = w[j].v[h] - w[k].v[h], d = dx * dx + dy * dy; if (d < bd) { bd = d; best = j; } }
            fprintf(out, "pass %u: head %06X came down %d px in %u passes; step %d; trace (passes ago: x v dir next), then the nearest head%c", pass, s, v - w[k].v0, pass - w[k].t0, (int)(int16_t)gm_rd16(g, A5G(g, G_STEP)), 10);
            for (int t = HL - 1; t >= 0; t -= 2) {
                int hh = (int)((pass + HL - (uint32_t)t) % HL);
                fprintf(out, " -%2d: %3d %3d d%d n%d", t, w[k].x[hh], w[k].v[hh], w[k].d[hh], w[k].n[hh]);
                if (best >= 0) fprintf(out, "   | %3d %3d d%d n%d", w[best].x[hh], w[best].v[hh], w[best].d[hh], w[best].n[hh]);
                fputc(10, out);
            }
            if (g_pede_log) fflush(g_pede_log);
            w[k].v0 = v; w[k].t0 = pass;
        }
    }
    for (int i = 0; i < NW; i++) if (!w[i].alive) w[i].s = 0;
}

/* test (APEIRON_PEDEFUZZ): replaces the Pentipede by `n` single heads at or near a side wall, with directions and rows taken from the seed */
void pede_fuzz_setup(GameCtx *g, uint32_t seed)
{
    uint32_t list = gm_rd32(g, A5G(g, G_PEDE_LIST)), old[64]; int n_old = 0;
    for (uint32_t s = gm_rd32(g, list + L_FIRST); s && n_old < 64; s = gm_rd32(g, s + S_NEXT)) old[n_old++] = s;
    for (int i = 0; i < n_old; i++) {
        int32_t h = (asr16(gm_rd32(g, old[i] + F_POS_H)) + 8) >> 4, v = (asr16(gm_rd32(g, old[i] + F_POS_V)) + 8) >> 4;
        sprite_deactivate(g, old[i]);
        gm_wr32(g, cell32(g, G_PEDE_GRID, h, v), 0);
    }
    gm_wr16(g, A5G(g, G_PEDE_ALIVE), 0);
    uint32_t r = seed * 2654435761u + 12345u;
    #define NEXT_R() (r = r * 1664525u + 1013904223u, (r >> 8))
    int n = 2 + (int)(NEXT_R() % 3), right = (int)(NEXT_R() & 1);
    int32_t row0 = (int32_t)(NEXT_R() % 4) * 16;
    for (int i = 0; i < n; i++) {
        int32_t h = right ? 0x1A0 - (int32_t)(NEXT_R() % 28) : (int32_t)(NEXT_R() % 28), v = row0 + (int32_t)(NEXT_R() % 3) * 16 + (int32_t)(NEXT_R() % 2) * (int32_t)(NEXT_R() % 12);
        if (gm_rd32(g, cell32(g, G_PEDE_GRID, (h + 8) >> 4, (v + 8) >> 4))) continue;
        gm_wr16(g, A5G(g, G_PEDE_ALIVE), (uint16_t)(gm_rd16(g, A5G(g, G_PEDE_ALIVE)) + 1));
        uint32_t s = new_sprite(g, -0x634, TAG_PHED, -0x648);
        uint32_t rec = new_segment_record(g, (uint8_t)(1 + (NEXT_R() & 1)), 0);
        gm_wr16(g, rec + 0x88, 1);
        gm_wr8(g, rec + R_DIVE, 0);
        gm_wr32(g, s + F_SCRATCH, rec);
        place_segment(g, s, h, v);
    }
    #undef NEXT_R
}

/* test (APEIRON_COINFIXTEST): 3000 coin bursts; counts the coins that fly up with no sideways speed (these never fall in the original) */
int coin_fix_test(GameCtx *g)
{
    int stuck = 0, total = 0;
    for (int i = 0; i < 3000; i++) {
        spawn_coins(g, gm_rd32(g, A5G(g, G_PLAYER)), gm_rd32(g, A5G(g, G_CLASS_COIN)));
        uint32_t list = gm_rd32(g, A5G(g, G_LIST_BAD));
        for (uint32_t s = gm_rd32(g, list + L_FIRST); s; s = gm_rd32(g, s + S_NEXT))
            if (gm_rd32(g, s) == TAG_YUMY && (int32_t)gm_rd32(g, s + F_VEL_V) <= 0) { total++; if (gm_rd32(g, s + F_VEL_H) == 0) stuck++; }
        for (uint32_t s = gm_rd32(g, list + L_FIRST); s; s = gm_rd32(g, s + S_NEXT))
            if (gm_rd32(g, s) == TAG_YUMY) gm_wr32(g, s + F_VEL_V, 0x40000u);          /* so the next burst's coins are the only ones counted */
    }
    fprintf(stderr, "coins thrown up: %d, with no sideways speed: %d\n", total, stuck);
    return stuck;
}
