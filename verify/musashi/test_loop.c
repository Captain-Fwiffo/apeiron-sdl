/* Differential test of one pass of the in-game loop (seg3_123C, entered at its body 0x41244) against game_pass() in src/creatures.c.
 *
 * A pass runs the engine move pass, the mushroom dirty marking, the per-step update, the shot hit handling, the draw pass and, when no pede
 * segment is left, the end of the wave and a new round. Everything the Mac side would do (sound, drawing, palette, Toolbox) is stubbed on both sides and
 * compared by address and arguments; everything ported runs for real on both sides. The state is random: the application globals are filled with random
 * bytes and then the pointers, lists, classes, grids and sprites are set up properly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/creatures.h"
#include "harness.h"

#define ENGINE   0x3A0000u
#define BOUNDS   0x3A1000u
#define LISTS    0x3A2000u
#define GRID     0x310400u
#define GRID2    0x315400u
#define CELLS    0x320000u
#define RECS     0x350000u
#define ROWS     0x3B0000u
#define HANDLES  0x3C0000u
#define CLASSDATA 0x3C1000u
#define SPRITES  0x3E0000u
#define HEAP     0x500000u
#define STUB_B   0x600000u
#define STUB_C   0x610000u
#define SEED     0xA01C2u
#define NCLASS   8
#define NFRAMES  96

static uint32_t xs = 0x0BADC0DEu;
static uint32_t rnd32(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }
static int rr(int lo, int hi) { return lo + (int)(rnd32() % (uint32_t)(hi - lo + 1)); }

typedef struct { uint32_t base, len; } Region;
static Region g_regions[24];
static int g_nreg;
static uint8_t *cmem;
static GameCtx ctx;
static uint32_t g_playing;

static CallRec c_calls[160];
static int c_ncalls;
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* ported routines run natively on the C side, exactly as they run natively in the 68k code */
static uint32_t c_ext(void *ud, int kind, const uint8_t *raw, int n)
{
    (void)ud;
    uint32_t a = n >= 4 ? be32(raw) : 0, b = n >= 8 ? be32(raw + 4) : 0;
    switch ((uint32_t)kind) {
    case EXT_EXPLOSION: spawn_explosion(&ctx, a, b); return 0;
    case EXT_MUSHROOM: spawn_mushroom(&ctx, (int16_t)(raw[0] << 8 | raw[1]), (int16_t)(raw[2] << 8 | raw[3])); return 0;
    case 0x4363A: spawn_sparks(&ctx, a); return 0;
    case 0x43E26: spawn_explosion(&ctx, a, b); return 0;
    case 0x40902: engine_activate(&ctx, a); return 0;
    case 0x42128: add_pede_head(&ctx); return 0;
    case 0x43A36: spawn_psychedelic(&ctx); return 0;
    case 0x43B02: spawn_multiplier(&ctx); return 0;
    case 0x43BD8: spawn_fodder(&ctx); return 0;
    case 0x42256: fire_shot(&ctx); return 0;
    case 0x42334: timed_launches(&ctx); return 0;
    case 0x42048: shield_start(&ctx); return 0;
    case 0x420CC: house_clean(&ctx); return 0;
    case 0x4299A: build_round(&ctx); return 0;
    case 0x400EA: engine_move_pass(&ctx); return 0;
    case 0x43348: return (cmem[0x17B] & 2) ? 0xFFu : 0u;
    default: break;
    }
    if (c_ncalls >= 160) return 0;
    CallRec *c = &c_calls[c_ncalls++];
    c->addr = kind >= 0x1000 ? (uint32_t)kind : kind == EXT_SOUND ? g_a5 + 0x3C2u : 0;
    c->n = n;
    if (n) memcpy(c->raw, raw, (size_t)n);
    return (uint32_t)kind == g_a5 + 0x3D2u ? g_playing : 0;
}

static uint32_t stub_ret(uint32_t addr, const uint8_t *raw) { (void)raw; return addr == g_a5 + 0x3D2u ? g_playing : 0; }

static void w8(uint32_t a, uint32_t v) { g_mem[a] = (uint8_t)v; }
static void g5(int off, uint32_t v, int bytes)
{
    uint32_t a = g_a5 + (uint32_t)off;
    if (bytes == 4) wr32(a, v); else if (bytes == 2) wr16(a, (uint16_t)v); else g_mem[a] = (uint8_t)v;
}
static uint32_t fixed(int lo, int hi) { return ((uint32_t)rr(lo, hi) << 16) | (rnd32() & 0xFFFF); }

static void build_class(int k)
{
    uint32_t h = HANDLES + (uint32_t)k * 4, d = CLASSDATA + (uint32_t)k * 0x800;
    wr32(h, d);
    for (uint32_t i = 0; i < 0x800; i += 4) wr32(d + i, rnd32());
    wr16(d + 4, (uint16_t)rr(3, 8));
    wr16(d + 6, (uint16_t)rr(0, 3)); wr16(d + 8, (uint16_t)rr(0, 3));
    wr16(d + 0xA, (uint16_t)rr(8, 24)); wr16(d + 0xC, (uint16_t)rr(8, 24));
    wr32(d + 0xE, 0x100); wr32(d + 0x12, 0x300);
    for (int f = 0; f < NFRAMES; f++) {
        wr32(d + 0x100 + (uint32_t)f * 4, STUB_B + (uint32_t)k * 0x400 + (uint32_t)f * 8 - d);
        wr32(d + 0x300 + (uint32_t)f * 4, STUB_C + (uint32_t)k * 0x400 + (uint32_t)f * 8 - d);
    }
}

static uint32_t nspr;
static const uint32_t kMoveCbs[] = { 0x40B0E, 0x44AE6, 0x44A9A, 0x44832, 0x440A4, 0x454DA, 0x4454C, 0x45216 };
static const uint32_t kFrameCbs[] = { 0x40BD8, 0x44310, 0x4492E, 0x447C2, 0x440AC, 0x44260 };

static uint32_t add_sprite(uint32_t list, uint32_t tag)
{
    uint32_t s = SPRITES + 0x100 + nspr++ * 0x100;
    for (uint32_t i = 0; i < 0x100; i += 4) wr32(s + i, rnd32());
    wr32(s, tag);
    wr32(s + 4, HANDLES + (uint32_t)rr(0, NCLASS - 1) * 4);
    int16_t top = (int16_t)rr(-10, 490), left = (int16_t)rr(-10, 430);
    int16_t h = (int16_t)rr(8, 24), w = (int16_t)rr(8, 24);
    wr16(s + 8, (uint16_t)top); wr16(s + 0xA, (uint16_t)left); wr16(s + 0xC, (uint16_t)(top + h)); wr16(s + 0xE, (uint16_t)(left + w));
    int16_t otop = (int16_t)(top + rr(-6, 6)), oleft = (int16_t)(left + rr(-6, 6));
    wr16(s + 0x10, (uint16_t)otop); wr16(s + 0x12, (uint16_t)oleft); wr16(s + 0x14, (uint16_t)(otop + h)); wr16(s + 0x16, (uint16_t)(oleft + w));
    wr16(s + 0x18, (uint16_t)w); wr16(s + 0x1A, (uint16_t)h);
    static const uint8_t flag[4] = { 0, 1, 0xFF, 0 };
    w8(s + 0x1C, rr(0, 3) ? 1 : 0); w8(s + 0x1D, flag[rr(0, 3)]); w8(s + 0x1E, flag[rr(0, 3)]); w8(s + 0x1F, rr(0, 7) == 0);
    wr32(s + 0x20, kMoveCbs[rr(0, 7)]); wr32(s + 0x42, kFrameCbs[rr(0, 5)]);
    static const uint16_t iv[] = { 0xFFFF, 0, 1, 2, 5 };
    uint32_t now = rd32(0x16A);
    wr16(s + 0x28, iv[rr(0, 4)]); wr16(s + 0x50, iv[rr(0, 4)]);
    wr32(s + 0x24, now - (uint32_t)rr(0, 8)); wr32(s + 0x4C, now - (uint32_t)rr(0, 8));
    wr16(s + 0x46, (uint16_t)rr(2, 8)); wr16(s + 0x48, (uint16_t)rr(1, 8)); wr16(s + 0x4A, (uint16_t)rr(1, 8));
    wr32(s + 0x2A, fixed(0, 420)); wr32(s + 0x2E, fixed(0, 500));
    wr32(s + 0x3A, rr(0, 3) ? fixed(-3, 3) : 0); wr32(s + 0x3E, rr(0, 3) ? fixed(-3, 3) : 0);
    wr32(s + 0x52, rnd32() & 0xFFFF);
    uint32_t last = rd32(list + 8);
    wr32(s + 0x56, last); wr32(s + 0x5A, 0);
    if (last) wr32(last + 0x5A, s);
    if (!rd32(list + 4)) wr32(list + 4, s);
    wr32(list + 8, s);
    return s;
}

static void setup(void)
{
    memset(&g_mem[ENGINE], 0, 0x100);
    memset(&g_mem[LISTS], 0, 0x200);
    memset(&g_mem[0x30F000], 0, 0x3000);
    memset(&g_mem[0x314000], 0, 0x3000);
    memset(&g_mem[CELLS], 0, 0x10000);
    memset(&g_mem[SPRITES], 0, 0x6000);
    memset(&g_mem[RECS], 0, 0x2000);
    memset(&g_mem[HEAP], 0, 0x8000);
    for (uint32_t i = 0; i < 0x8000; i += 4) wr32(ROWS + i, rnd32());
    for (int k = 0; k < NCLASS; k++) build_class(k);
    g_heap_next = HEAP; ctx.heap_next = HEAP;
    /* the application globals below A5: random bytes, then the meaningful ones */
    for (uint32_t a = A5_WORLD; a < g_a5; a++) g_mem[a] = (uint8_t)rnd32();
    for (uint32_t a = 0x170; a < 0x180; a++) g_mem[a] = (uint8_t)rnd32();
    uint32_t ticks = rnd32() & 0xFFFFF;
    wr32(0x16A, ticks);
    g5(-0x75A, 1, 1); g5(-0x758, ENGINE, 4);
    wr32(ENGINE + 0x14, BOUNDS);
    wr16(BOUNDS + 0x14, (uint16_t)rr(400, 500)); wr16(BOUNDS + 0x16, (uint16_t)rr(400, 460));
    wr32(ENGINE + 0x18, ROWS + 0x1400); wr32(ENGINE + 0x1C, ROWS + 0x3400); wr32(ENGINE + 0x20, ROWS + 0x5400);
    wr16(ENGINE + 0x24, (uint16_t)rr(0, 40)); wr16(ENGINE + 0x26, (uint16_t)rr(0, 40)); wr16(ENGINE + 0x28, (uint16_t)rr(0, 40));
    wr16(ENGINE + 0x2A, (uint16_t)rr(0, 3)); wr32(ENGINE + 0x2C, ticks - (uint32_t)rr(0, 6));
    w8(ENGINE + 0x30, rr(0, 3) == 0); w8(ENGINE + 0x31, rr(0, 3) == 0);
    wr16(ENGINE + 0x3A, (uint16_t)rr(-8, 8)); wr16(ENGINE + 0x3C, (uint16_t)rr(-8, 8));
    /* lists */
    uint32_t le = LISTS, lm = LISTS + 0x20, lp = LISTS + 0x40, lx = LISTS + 0x60, ls = LISTS + 0x80;
    uint32_t order[5] = { ls, lp, lx, le, lm };
    for (int i = 0; i < 5; i++) {
        wr32(order[i], rnd32()); w8(order[i] + 0xC, rr(0, 5) ? 1 : 0);
        wr32(order[i] + 0xE, i ? order[i - 1] : 0);
        if (i) wr32(order[i - 1] + 0x12, order[i]);
    }
    wr32(ENGINE + 4, order[0]); wr32(ENGINE + 8, order[4]);
    g5(-0x63C, le, 4); g5(-0x64C, lm, 4); g5(-0x648, lp, 4); g5(-0x640, lx, 4); g5(-0x644, ls, 4);
    for (int off = -0x638; off >= -0x628 - 0x10 || off >= -0x640; off -= 4) { /* placeholder: replaced below */ if (off < -0x640) break; }
    static const int cls_off[] = { -0x638, -0x634, -0x630, -0x62C, -0x628, -0x624, -0x620, -0x61C, -0x618, -0x614, -0x610, -0x60C, -0x608, -0x604,
                                   -0x600, -0x5FC, -0x5F8, -0x5F4, -0x5F0, -0x5EC, -0x5E8 };
    for (size_t i = 0; i < sizeof cls_off / sizeof cls_off[0]; i++) g5(cls_off[i], HANDLES + (uint32_t)rr(0, NCLASS - 1) * 4, 4);
    g5(-0x6F0, GRID, 4); g5(-0x6EC, GRID2, 4);
    /* handle chains used by the palette routines */
    for (int i = 0; i < 4; i++) { wr32(SPRITES + 0x5C00 + (uint32_t)i * 4, SPRITES + 0x5D00 + (uint32_t)i * 0x20); wr32(SPRITES + 0x5D00 + (uint32_t)i * 0x20, SPRITES + 0x5E00 + (uint32_t)i * 0x20); }
    g5(-0x4C4, SPRITES + 0x5C00, 4); g5(-0x71C, SPRITES + 0x5C04, 4);
    g5(-0x728, SPRITES + 0x5C00, 4); g5(-0x724, SPRITES + 0x5C04, 4); g5(-0x720, SPRITES + 0x5C08, 4);
    /* state that needs sensible ranges */
    g5(-0x6DE, ticks - (uint32_t)rr(0, 3), 4); g5(-0x6DA, rr(0, 1) ? ticks : rnd32(), 4);
    g5(-0x692, ticks - (uint32_t)rr(0, 4), 4);
    g5(-0x710, ticks - (uint32_t)rr(0, 12), 4); g5(-0x714, ticks - (uint32_t)rr(0, 1000), 4);
    g5(-0x734, (uint32_t)rr(1, 40), 2); g5(-0x732, (uint32_t)rr(0, 40), 2);
    g5(-0x702, rr(0, 2) ? (uint32_t)rr(0, 60000) : (uint32_t)rr(0, 3000000), 4);
    g5(-0x704, (uint32_t)(rr(0, 3) ? 1 : 10), 2); g5(-0x706, (uint32_t)(rr(0, 3) ? 1 : 10), 4);
    g5(-0x6E6, (uint32_t)rr(-1, 3) , 2);
    if (rr(0, 3)) g5(-0x6E6, (uint32_t)rr(1, 12), 2);
    g5(-0x729, (uint32_t)rr(0, 1), 1); g5(-0x6CC, (uint32_t)rr(0, 4), 2); g5(-0x6CA, (uint32_t)rr(1, 5), 2);
    g5(-0x66A, (uint32_t)rr(0, 1), 1); g5(-0x730, 1, 1); g5(-0x4D2, 0, 1);
    g5(-0x716, (uint32_t)rr(0, 1), 1); g5(-0x715, (uint32_t)rr(0, 1), 1); g5(-0x718, (uint32_t)rr(0, 3), 2);
    g5(-0x708, (uint32_t)rr(0, 1), 1);
    g5(-0x6A8, (uint32_t)(rr(0, 4) == 0), 1);
    g5(-0x6D2, (uint32_t)rr(0, 1), 1);
    g5(-0x6E8, (uint32_t)rr(1, 20), 2);
    g5(-0x654, (uint32_t)rr(0, 40), 2); g5(-0x6A2, (uint32_t)rr(0, 25), 2);
    g5(-0x6C4, (uint32_t)rr(0, 6), 4); g5(-0x6C8, ticks - (uint32_t)rr(0, 10), 4);
    g5(-0x6D6, (uint32_t)rr(20, 250), 4); g5(-0x6D0, ticks - (uint32_t)rr(0, 300), 4);
    g5(-0x6A6, ticks + (uint32_t)rr(-40, 10), 4); g5(-0x6B6, rr(0, 1) ? 0 : ticks + (uint32_t)rr(-5, 3), 4);
    g5(-0x6BA, ticks + (uint32_t)rr(-5, 3), 4); g5(-0x6BE, (uint32_t)(rr(0, 2) == 0), 2);
    g5(-0x686, rr(0, 1) ? 0 : ticks + (uint32_t)rr(-3, 3), 4); g5(-0x6AE, rr(0, 1) ? 0 : ticks + (uint32_t)rr(-3, 3), 4);
    g5(-0x6B2, rr(0, 1) ? 0 : ticks + (uint32_t)rr(-3, 3), 4);
    g5(-0x6E2, (uint32_t)rr(0, 9), 2); g5(-0x6E4, (uint32_t)rr(0, 30), 2); g5(-0x6BC, (uint32_t)rr(1, 12), 2);
    g5(-0x672, ticks - (uint32_t)rr(0, 1900), 4);
    g5(-0x6FA, (uint32_t)rr(-1, 9), 2); g5(-0x6F8, (uint32_t)rr(-1, 9), 2);
    g5(-0x6F6, (uint32_t)(rr(0, 3) == 0), 1); g5(-0x6F4, ticks - (uint32_t)rr(0, 300), 4);
    g5(-0x6FE, rnd32() & 0xFFFFF, 4); g5(-0x696, ticks - (uint32_t)rr(0, 4), 4);
    g5(-0x68E, rnd32() & 0xFFFFF, 4); g5(-0x67C, ticks + (uint32_t)rr(-10, 10), 4);
    g5(-0x682, (uint32_t)rr(0, 300), 4); g5(-0x67E, (uint32_t)rr(0, 300), 2);
    g5(-0x6C0, (uint32_t)rr(1, 4) * 200, 2);
    g5(-0x65E, (uint32_t)rr(380, 420), 2); g5(-0x662, (uint32_t)rr(0, 20), 2); g5(-0x660, (uint32_t)rr(440, 480), 2); g5(-0x664, (uint32_t)rr(340, 400), 2);
    g5(-0x650, (uint32_t)rr(400, 490), 2); g5(-0x64E, (uint32_t)rr(380, 440), 2); g5(-0x652, (uint32_t)rr(0, 30), 2);
    g5(-0x742, (uint32_t)rr(300, 450), 2); g5(-0x746, (uint32_t)rr(0, 20), 2); g5(-0x744, (uint32_t)rr(440, 480), 2); g5(-0x748, (uint32_t)rr(0, 20), 2);
    g5(-0x41C, rnd32(), 2); g5(-0x41A, rnd32(), 2); g5(-0x674, (uint32_t)rr(1, 5), 2);
    g_playing = (uint32_t)(rr(0, 1) ? 0xFFu : 0u) | (rnd32() & 0xFFFFFF00u);
    wr32(SEED, rnd32());

    /* the mushroom grid and the sprites */
    int idx = 0, density = rr(0, 60);
    for (int h = 0; h < 27; h++)
        for (int v = 0; v < 30; v++, idx++) {
            if (rr(0, 99) >= density) continue;
            uint32_t rec = CELLS + (uint32_t)idx * 0x50;
            for (uint32_t k = 0; k < 0x50; k += 4) wr32(rec + k, rnd32());
            wr32(rec, 0x7368726Du); wr16(rec + 0x48, (uint16_t)rr(0, 16)); wr16(rec + 0x46, (uint16_t)rr(2, 8));
            g_mem[rec + 0x1C] = rr(0, 5) ? 1 : 0;
            wr32(GRID + (uint32_t)h * 0x78 + (uint32_t)v * 4, rec);
        }
    nspr = 0;
    uint32_t PL = add_sprite(ls, 0x73687472u); g5(-0x59C, PL, 4);
    wr32(PL + 0x2A, fixed(0, 420)); wr32(PL + 0x2E, fixed(330, 470)); g_mem[PL + 0x1C] = 1;
    uint32_t SC = add_sprite(lx, 0x73636F72u), GE = add_sprite(lx, 0x6765636Bu);
    g5(-0x590, SC, 4); g5(-0x594, GE, 4); wr16(SC + 0x18, 20); wr16(SC + 0x1A, 20); wr16(GE + 0x18, 20); wr16(GE + 0x1A, 20);
    static const uint32_t tags_s[] = { 0x73686F74u, 0x7368656Cu, 0x64656164u };
    static const uint32_t tags_x[] = { 0x666C6561u, 0x6D756C74u, 0x70737963u, 0x79756D79u, 0x666F6472u, 0x726F6F74u, 0x6F746865u };
    for (int i = rr(0, 3); i > 0; i--) add_sprite(ls, tags_s[rr(0, 2)]);
    for (int i = rr(0, 3); i > 0; i--) add_sprite(lx, tags_x[rr(0, 6)]);
    for (int i = rr(0, 3); i > 0; i--) add_sprite(le, 0x6578706Cu);
    uint32_t prev = 0;
    for (int i = rr(0, 4); i > 0; i--) {
        uint32_t s = add_sprite(lp, rr(0, 3) ? 0x70626F64u : 0x70686564u);
        uint32_t rec = RECS + (uint32_t)i * 0x100;
        for (uint32_t k = 0; k < 0x100; k += 4) wr32(rec + k, rnd32());
        wr32(s + 0x52, rec); g_mem[rec + 0x8A] = (uint8_t)rr(0, 1); g_mem[rec + 0x84] = (uint8_t)rr(0, 5); g_mem[rec + 0x85] = (uint8_t)rr(0, 5);
        wr32(rec + 0x7C, prev); wr32(rec + 0x80, 0);
        if (prev) wr32(rd32(prev + 0x52) + 0x80, s);
        prev = s;
        wr32(s + 0x20, 0x440A4);
    }
    for (int i = rr(0, 4); i > 0; i--) add_sprite(lm, 0x7368726Du);
}

static void snapshot_to_c(void) { for (int i = 0; i < g_nreg; i++) memcpy(&cmem[g_regions[i].base], &g_mem[g_regions[i].base], g_regions[i].len); }

static int compare_calls(long trial);
static int compare(long trial)
{
    if (getenv("CALLS_FIRST") && compare_calls(trial)) return 1;
    for (int i = 0; i < g_nreg; i++) {
        const uint8_t *a = &g_mem[g_regions[i].base], *b = &cmem[g_regions[i].base];
        if (memcmp(a, b, g_regions[i].len) == 0) continue;
        for (uint32_t k = 0; k < g_regions[i].len; k++)
            if (a[k] != b[k]) { printf("  trial %ld: memory differs at %06X (68k %02X, C %02X)\n", trial, g_regions[i].base + k, a[k], b[k]); return 1; }
    }
    if (g_ncalls != c_ncalls) {
        printf("  trial %ld: %d logged calls in 68k, %d in C\n", trial, g_ncalls, c_ncalls);
        if (getenv("SHOW")) for (int i = 0; i < g_ncalls || i < c_ncalls; i++) printf("     %2d  68k %06X   C %06X\n", i, i < g_ncalls ? g_calls[i].addr : 0, i < c_ncalls ? c_calls[i].addr : 0);
        return 1;
    }
    for (int i = 0; i < g_ncalls; i++)
        if (g_calls[i].addr != c_calls[i].addr || g_calls[i].n != c_calls[i].n || memcmp(g_calls[i].raw, c_calls[i].raw, (size_t)g_calls[i].n)) {
            printf("  trial %ld: call %d differs (68k %06X n=%d, C %06X n=%d)\n", trial, i, g_calls[i].addr, g_calls[i].n, c_calls[i].addr, c_calls[i].n);
            return 1;
        }
    return 0;
}

static int compare_calls(long trial)
{
    if (g_ncalls != c_ncalls) { printf("  trial %ld: %d logged calls in 68k, %d in C\n", trial, g_ncalls, c_ncalls); for (int i = 0; i < g_ncalls || i < c_ncalls; i++) printf("     %2d  68k %06X   C %06X\n", i, i < g_ncalls ? g_calls[i].addr : 0, i < c_ncalls ? c_calls[i].addr : 0); return 1; }
    for (int i = 0; i < g_ncalls; i++)
        if (g_calls[i].addr != c_calls[i].addr || g_calls[i].n != c_calls[i].n || memcmp(g_calls[i].raw, c_calls[i].raw, (size_t)g_calls[i].n)) { printf("  trial %ld: call %d differs\n", trial, i); return 1; }
    return 0;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../../../apeiron_work";
    int trials = argc > 2 ? atoi(argv[2]) : 20000;
    if (image_load(dir)) return 2;
    cmem = (uint8_t *)calloc(MEM_SIZE, 1);
    ctx.mem = cmem; ctx.a5 = g_a5; ctx.ext = c_ext; ctx.native_overlap = 1;
    g_stub_cb = stub_ret;
    g_regions[g_nreg++] = (Region){ A5_WORLD, g_world_len };
    g_regions[g_nreg++] = (Region){ ENGINE, 0x100 };
    g_regions[g_nreg++] = (Region){ BOUNDS, 0x100 };
    g_regions[g_nreg++] = (Region){ LISTS, 0x200 };
    g_regions[g_nreg++] = (Region){ 0x30F000, 0x3000 };
    g_regions[g_nreg++] = (Region){ 0x314000, 0x3000 };
    g_regions[g_nreg++] = (Region){ CELLS, 0x10000 };
    g_regions[g_nreg++] = (Region){ RECS, 0x2000 };
    g_regions[g_nreg++] = (Region){ ROWS, 0x8000 };
    g_regions[g_nreg++] = (Region){ HANDLES, 0x6000 };
    g_regions[g_nreg++] = (Region){ SPRITES, 0x6000 };
    g_regions[g_nreg++] = (Region){ HEAP, 0x8000 };
    g_regions[g_nreg++] = (Region){ 0xA01B0, 0x20 };
    g_regions[g_nreg++] = (Region){ 0x16A, 4 };
    g_regions[g_nreg++] = (Region){ 0x170, 0x10 };

    harness_clear_stubs();
    harness_add_stub(g_a5 + 0x3C2u, 10); harness_add_stub(g_a5 + 0x3D2u, 4); harness_add_stub(g_a5 + 0x3DAu, 2); harness_add_stub(g_a5 + 0x4E2u, 4);
    harness_add_stub(g_a5 + 0x36Au, 5); harness_add_stub(g_a5 + 0x39Au, 6); harness_add_stub(g_a5 + 0x3E2u, 12);
    static const struct { uint32_t addr; int n; } st[] = { { 0x415E4, 9 }, { 0x41428, 0 }, { 0x4157A, 0 }, { 0x42596, 0 }, { 0x426C2, 2 }, { 0x42836, 0 },
                                                          { 0x40098, 0 }, { 0x405CE, 0 }, { 0x400C2, 0 }, { 0x432F8, 0 } };
    for (size_t i = 0; i < sizeof st / sizeof st[0]; i++) harness_add_stub(st[i].addr, st[i].n);
    for (int k = 0; k < NCLASS; k++)
        for (int f = 0; f < NFRAMES; f++) {
            harness_add_stub(STUB_B + (uint32_t)k * 0x400 + (uint32_t)f * 8, -3);
            harness_add_stub(STUB_C + (uint32_t)k * 0x400 + (uint32_t)f * 8, -2);
        }

    int bad = 0, builds = 0;
    for (int t = 0; t < trials && bad < 5; t++) {
        setup();
        snapshot_to_c();
        c_ncalls = 0;
        g_extra_stack = 12;
        extern int g_trace; g_trace = getenv("TT") && t == atoi(getenv("TT"));
        m68k_call(0x41244, NULL, NULL, 0, 3000000);
        g_trace = 0;
        if (g_instr_count >= 3000000) { printf("  trial %d: the 68k run did not finish\n", t); continue; }
        game_pass(&ctx);
        if ((int16_t)rd16(g_a5 - 0x6E6) > 0 || 1) builds += 0;
        if (compare(t)) bad++;
    }
    printf("seg3_123C one loop pass         %d trials, %d mismatches\n", trials, bad);
    (void)builds;
    return bad ? 1 : 0;
}
