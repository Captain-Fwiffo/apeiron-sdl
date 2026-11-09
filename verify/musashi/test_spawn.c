/* Differential test of the effect and spawn routines of CODE 3 against src/creatures.c:
 *   seg3_3E26 explosion, seg3_31A0 mushroom, seg3_3558 fragments, seg3_363A sparks, seg3_3740 / seg3_3854 score digits.
 * They allocate sprites with the (already tested) sprite allocator, so every new sprite record, the list links, the heap use and the RNG seed are compared.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/creatures.h"
#include "harness.h"

#define ENGINE   0x3A0000u
#define LIST_E   0x3A2000u
#define LIST_M   0x3A2020u
#define GRID     0x310400u
#define GRID2    0x315400u
#define CELLS    0x320000u
#define HANDLES  0x3C0000u
#define CLASSDATA 0x3C1000u
#define SRC      0x3E0000u
#define OLD      0x3E0100u
#define HEAP     0x500000u
#define SEED     0xA01C2u

static uint32_t xs = 0x7F4A7C15u;
static uint32_t rnd32(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }
static int rr(int lo, int hi) { return lo + (int)(rnd32() % (uint32_t)(hi - lo + 1)); }

typedef struct { uint32_t base, len; } Region;
static Region g_regions[16];
static int g_nreg;
static uint8_t *cmem;
static GameCtx ctx;

static uint32_t g_playing;
static uint32_t stub_ret(uint32_t addr, const uint8_t *raw) { (void)raw; return addr == g_a5 + 0x3D2u ? g_playing : 0; }
static CallRec c_calls[16];
static int c_ncalls;
static uint32_t c_ext(void *ud, int kind, const uint8_t *raw, int n)
{
    (void)ud;
    if (c_ncalls >= 16) return 0;
    CallRec *c = &c_calls[c_ncalls++];
    c->addr = kind >= 0x1000 ? (uint32_t)kind : kind == EXT_SOUND ? g_a5 + 0x3C2u : 0;
    c->n = n;
    if (n) memcpy(c->raw, raw, (size_t)n);
    return kind == (int)(g_a5 + 0x3D2u) ? g_playing : 0;
}

static void g5(int off, uint32_t v, int bytes)
{
    uint32_t a = g_a5 + (uint32_t)off;
    if (bytes == 4) wr32(a, v); else if (bytes == 2) wr16(a, (uint16_t)v); else g_mem[a] = (uint8_t)v;
}

static void build_class(int k)
{
    uint32_t h = HANDLES + (uint32_t)k * 4, d = CLASSDATA + (uint32_t)k * 0x400;
    wr32(h, d);
    for (uint32_t i = 0; i < 0x400; i += 4) wr32(d + i, rnd32());
    wr16(d + 4, (uint16_t)rr(3, 8));
    wr16(d + 6, (uint16_t)rr(0, 3)); wr16(d + 8, (uint16_t)rr(0, 3));
    wr16(d + 0xA, (uint16_t)rr(8, 40)); wr16(d + 0xC, (uint16_t)rr(8, 40));
}

static void setup(void)
{
    memset(&g_mem[ENGINE], 0, 0x100);
    memset(&g_mem[LIST_E], 0, 0x100);
    memset(&g_mem[0x30F000], 0, 0x3000);
    memset(&g_mem[0x314000], 0, 0x3000);
    memset(&g_mem[CELLS], 0, 0x10000);
    memset(&g_mem[HEAP], 0, 0x4000);
    for (int k = 0; k < 4; k++) build_class(k);
    g_heap_next = HEAP; ctx.heap_next = HEAP;
    g5(-0x75A, 1, 1); g5(-0x758, ENGINE, 4);
    wr16(ENGINE + 0x2A, (uint16_t)rr(0, 5));
    wr32(LIST_E, rnd32()); wr32(LIST_M, rnd32());
    wr32(ENGINE + 4, LIST_E); wr32(ENGINE + 8, LIST_M);
    wr32(LIST_E + 0x12, LIST_M); wr32(LIST_M + 0xE, LIST_E);
    g5(-0x63C, LIST_E, 4); g5(-0x64C, LIST_M, 4);
    wr32(LIST_M + 0x20, rnd32()); g5(-0x640, LIST_M + 0x20, 4); g5(-0x5F8, HANDLES + 8, 4); g5(-0x5F0, HANDLES + 12, 4);
    g5(-0x638, HANDLES + 0, 4); g5(-0x61C, HANDLES + 4, 4); g5(-0x60C, HANDLES + 8, 4);
    g5(-0x618, HANDLES + 12, 4); g5(-0x614, HANDLES + 0, 4); g5(-0x610, HANDLES + 4, 4);
    g5(-0x628, HANDLES + 4, 4); g5(-0x634, HANDLES + 0, 4); wr32(LIST_M + 0x40, rnd32()); g5(-0x648, LIST_M + 0x40, 4); g5(-0x6EC, GRID2, 4);
    g5(-0x734, (uint32_t)rr(0, 30), 2); g5(-0x6BE, (uint32_t)rr(0, 5), 2); g5(-0x6E6, (uint32_t)rr(0, 9), 2); g5(-0x53C, rnd32(), 4);
    { uint32_t tk = rnd32() & 0xFFFFF; wr32(0x16A, tk); g5(-0x6DE, tk, 4); g5(-0x6D0, tk - (uint32_t)rr(0, 300), 4); g5(-0x6D6, (uint32_t)rr(25, 245), 4); }
    g5(-0x6E2, (uint32_t)rr(0, 9), 2); g5(-0x6E4, (uint32_t)rr(0, 30), 2);
    g5(-0x6F0, GRID, 4);
    wr32(SEED, rnd32());
    /* an existing sprite in each list, so the tail links are exercised */
    for (int i = 0; i < 2; i++) {
        uint32_t l = i ? LIST_M : LIST_E, s = OLD + (uint32_t)i * 0x100;
        for (uint32_t k = 0; k < 0x5E; k++) g_mem[s + k] = (uint8_t)rnd32();
        wr32(s + 0x5A, 0); wr32(s + 0x56, 0);
        if (rr(0, 1)) { wr32(l + 4, s); wr32(l + 8, s); }
    }
    /* the source sprite */
    for (uint32_t k = 0; k < 0x5E; k++) g_mem[SRC + k] = (uint8_t)rnd32();
    int16_t top = (int16_t)rr(-10, 500), left = (int16_t)rr(-10, 440);
    wr16(SRC + 0x18, (uint16_t)rr(8, 40)); wr16(SRC + 0x1A, (uint16_t)rr(8, 40));
    wr16(SRC + 8, (uint16_t)top); wr16(SRC + 0xA, (uint16_t)left);
    wr16(SRC + 0xC, (uint16_t)(top + 20)); wr16(SRC + 0xE, (uint16_t)(left + 20));
    wr32(SRC + 0x2A, ((uint32_t)rr(-10, 440) << 16) | (rnd32() & 0xFFFF)); wr32(SRC + 0x2E, ((uint32_t)rr(-10, 500) << 16) | (rnd32() & 0xFFFF));
    /* shield, house cleaning and launch state */
    {
        uint32_t tk = rd32(0x16A);
        uint32_t PL = SRC + 0xB00, SC = SRC + 0x900, GE = SRC + 0xA00;
        for (uint32_t k = 0; k < 0x5E; k++) { g_mem[PL + k] = (uint8_t)rnd32(); g_mem[SC + k] = (uint8_t)rnd32(); g_mem[GE + k] = (uint8_t)rnd32(); }
        wr32(PL + 0x2A, ((uint32_t)rr(0, 420) << 16) | (rnd32() & 0xFFFF)); wr32(PL + 0x2E, ((uint32_t)rr(300, 470) << 16) | (rnd32() & 0xFFFF));
        g_mem[SC + 0x1C] = (uint8_t)rr(0, 1); g_mem[SC + 0x1E] = (uint8_t)(rr(0, 4) == 0); g_mem[GE + 0x1C] = (uint8_t)rr(0, 1); g_mem[GE + 0x1E] = (uint8_t)(rr(0, 4) == 0);
        if (rr(0, 1)) { g_mem[SC + 0x1C] = 0; g_mem[GE + 0x1C] = 0; }
        wr16(SC + 0x18, 20); wr16(SC + 0x1A, 20); wr16(GE + 0x18, 20); wr16(GE + 0x1A, 20);
        g5(-0x59C, PL, 4); g5(-0x590, SC, 4); g5(-0x594, GE, 4); g5(-0x5EC, HANDLES + 4, 4); g5(-0x610, HANDLES + 8, 4);
        g5(-0x66A, (uint32_t)rr(0, 1), 1); g5(-0x672, tk - (uint32_t)rr(1500, 1900), 4); g5(-0x6A8, (uint32_t)(rr(0, 3) == 0), 1); g5(-0x66E, 0, 1);
        g5(-0x6A6, tk + (uint32_t)rr(-40, 10), 4); g5(-0x6B6, rr(0, 1) ? 0 : tk + (uint32_t)rr(-5, 3), 4);
        g5(-0x6A2, (uint32_t)rr(0, 25), 2); g5(-0x6BC, (uint32_t)rr(0, 12), 2); g5(-0x6BA, tk + (uint32_t)rr(-5, 3), 4);
        g5(-0x6BE, (uint32_t)(rr(0, 2) == 0 ? 1 : 0), 2); g5(-0x6E2, (uint32_t)rr(0, 3), 2); g5(-0x702, rr(0, 1) ? 100 : 60000, 4);
        g5(-0x6E4, (uint32_t)rr(0, 12), 2);
        static const int snd2[] = { -0x4FC, -0x54C, -0x508, -0x578, -0x514, -0x548 };
        for (size_t i = 0; i < sizeof snd2 / sizeof snd2[0]; i++) g5(snd2[i], rnd32(), 4);
        g_playing = (uint32_t)(rr(0, 1) ? 0xFFu : 0u) | (rnd32() & 0xFFFFFF00u);
        /* mushroom sprites in the mushroom list (for house cleaning) */
        uint32_t prev = 0;
        wr32(LIST_M + 4, 0); wr32(LIST_M + 8, 0);
        int nm = rr(0, 6);
        for (int i = 0; i < nm; i++) {
            uint32_t m = SRC + 0x200 + (uint32_t)i * 0x100;
            for (uint32_t k = 0; k < 0x5E; k++) g_mem[m + k] = (uint8_t)rnd32();
            wr32(m + 0x2A, ((uint32_t)rr(0, 430) << 16) | (rnd32() & 0xFFFF)); wr32(m + 0x2E, ((uint32_t)rr(0, 500) << 16) | (rnd32() & 0xFFFF));
            wr32(m + 0x56, prev); wr32(m + 0x5A, 0);
            if (prev) wr32(prev + 0x5A, m); else wr32(LIST_M + 4, m);
            wr32(LIST_M + 8, m); prev = m;
        }
    }
    /* round building state: classes, lists with old sprites, screen geometry */
    {
        wr32(LIST_M + 0x60, rnd32()); g5(-0x644, LIST_M + 0x60, 4);
        g5(-0x630, HANDLES + 12, 4); g5(-0x624, HANDLES + 4, 4); g5(-0x620, HANDLES + 8, 4); g5(-0x5EC, HANDLES + 0, 4);
        g5(-0x4C4, rnd32(), 4); g5(-0x716, (uint32_t)rr(0, 1), 1); g5(-0x715, (uint32_t)rr(0, 1), 1); g5(-0x718, rnd32(), 2);
        g5(-0x742, (uint32_t)rr(300, 450), 2); g5(-0x746, (uint32_t)rr(0, 20), 2); g5(-0x744, (uint32_t)rr(440, 480), 2); g5(-0x748, (uint32_t)rr(0, 20), 2);
        g5(-0x728, rnd32(), 4); g5(-0x58C, rnd32(), 4); g5(-0x708, (uint32_t)rr(0, 1), 1); g5(-0x66A, (uint32_t)rr(0, 1), 1);
        g5(-0x734, (uint32_t)rr(1, 40), 2); g5(-0x702, rr(0, 1) ? 100 : 50000, 4);
        { uint32_t tk = rd32(0x16A); g5(-0x6DE, tk, 4); g5(-0x6D6, (uint32_t)rr(20, 250), 4); (void)tk; }
        int slot = 0;
        for (int li = 0; li < 3; li++) {
            uint32_t list = li == 0 ? LIST_M + 0x40 : li == 1 ? LIST_M + 0x20 : LIST_M + 0x60;
            int n = rr(0, 3);
            uint32_t prevs = 0;
            wr32(list + 4, 0); wr32(list + 8, 0);
            for (int i = 0; i < n; i++) {
                uint32_t s = SRC + 0xC00 + (uint32_t)(slot++) * 0x100;
                for (uint32_t k = 0; k < 0x5E; k += 4) wr32(s + k, rnd32());
                wr32(s + 0x56, prevs); wr32(s + 0x5A, 0);
                if (prevs) wr32(prevs + 0x5A, s); else wr32(list + 4, s);
                wr32(list + 8, s); prevs = s;
            }
        }
    }
    /* grid cells: a few mushrooms */
    int idx = 0, density = rr(0, 50);
    for (int h = 0; h < 27; h++)
        for (int v = 0; v < 30; v++, idx++)
            if (rr(0, 99) < density) wr32(GRID + (uint32_t)h * 0x78 + (uint32_t)v * 4, CELLS + (uint32_t)idx * 0x50);
}

static void snapshot_to_c(void) { for (int i = 0; i < g_nreg; i++) memcpy(&cmem[g_regions[i].base], &g_mem[g_regions[i].base], g_regions[i].len); }

static int compare(const char *name, long trial)
{
    for (int i = 0; i < g_nreg; i++) {
        const uint8_t *a = &g_mem[g_regions[i].base], *b = &cmem[g_regions[i].base];
        if (memcmp(a, b, g_regions[i].len) == 0) continue;
        for (uint32_t k = 0; k < g_regions[i].len; k++)
            if (a[k] != b[k]) { printf("  %s trial %ld: memory differs at %06X (68k %02X, C %02X)\n", name, trial, g_regions[i].base + k, a[k], b[k]); return 1; }
    }
    if (g_ncalls != c_ncalls) { printf("  %s trial %ld: %d logged calls in 68k, %d in C\n", name, trial, g_ncalls, c_ncalls); return 1; }
    return 0;
}

enum { K_EXPL, K_MUSH, K_FRAG, K_SPARK, K_DIG_A, K_DIG_B, K_PSYC, K_MULT, K_FODDER, K_FLEA, K_HEAD, K_SHIELD, K_HOUSE, K_SH_FRAME, K_SH_MOVE, K_LAUNCH, K_ROUND, K_BUILD };
static const struct { const char *name; uint32_t addr; int kind; } kCases[] = {
    { "seg3_3E26 explosion", 0x43E26, K_EXPL }, { "seg3_31A0 mushroom", 0x431A0, K_MUSH }, { "seg3_3558 fragments", 0x43558, K_FRAG },
    { "seg3_363A sparks", 0x4363A, K_SPARK }, { "seg3_3740 score digits A", 0x43740, K_DIG_A }, { "seg3_3854 score digits B", 0x43854, K_DIG_B },
    { "seg3_3A36 psychedelic", 0x43A36, K_PSYC }, { "seg3_3B02 multiplier", 0x43B02, K_MULT }, { "seg3_3BD8 fodder", 0x43BD8, K_FODDER },
    { "seg3_3CEA fleas", 0x43CEA, K_FLEA }, { "seg3_2128 extra pede head", 0x42128, K_HEAD },
    { "seg3_2048 shield start", 0x42048, K_SHIELD }, { "seg3_20CC house cleaning", 0x420CC, K_HOUSE },
    { "seg3_4874 shield frame", 0x44874, K_SH_FRAME }, { "seg3_490E shield follow", 0x4490E, K_SH_MOVE }, { "seg3_2334 timed launches", 0x42334, K_LAUNCH }, { "seg3_30F6 clear playfield", 0x430F6, K_ROUND }, { "seg3_299A build round", 0x4299A, K_BUILD } };

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../../../apeiron_work";
    int trials = argc > 2 ? atoi(argv[2]) : 50000;
    if (image_load(dir)) return 2;
    cmem = (uint8_t *)calloc(MEM_SIZE, 1);
    ctx.mem = cmem; ctx.a5 = g_a5; ctx.ext = c_ext;
    g_regions[g_nreg++] = (Region){ A5_WORLD, g_world_len };
    g_regions[g_nreg++] = (Region){ ENGINE, 0x100 };
    g_regions[g_nreg++] = (Region){ LIST_E, 0x100 };
    g_regions[g_nreg++] = (Region){ 0x30F000, 0x3000 };
    g_regions[g_nreg++] = (Region){ 0x314000, 0x3000 };
    g_regions[g_nreg++] = (Region){ 0x16A, 4 };
    g_regions[g_nreg++] = (Region){ CELLS, 0x10000 };
    g_regions[g_nreg++] = (Region){ HANDLES, 0x2000 };
    g_regions[g_nreg++] = (Region){ SRC, 0x1600 };
    g_regions[g_nreg++] = (Region){ HEAP, 0x4000 };
    g_regions[g_nreg++] = (Region){ 0xA01B0, 0x20 };
    g_stub_cb = stub_ret;
    int total_bad = 0;
    for (size_t c = 0; c < sizeof kCases / sizeof kCases[0]; c++) {
        if (argc > 3 && !strstr(kCases[c].name, argv[3])) continue;
        harness_clear_stubs();
        harness_add_stub(g_a5 + 0x3C2u, 10);
        harness_add_stub(g_a5 + 0x3D2u, 4);
        harness_add_stub(0x432F8u, 0); harness_add_stub(g_a5 + 0x36Au, 5); harness_add_stub(g_a5 + 0x3E2u, 12); harness_add_stub(g_a5 + 0x39Au, 6);
        int bad = 0;
        for (int t = 0; t < trials && bad < 5; t++) {
            setup();
            snapshot_to_c();
            c_ncalls = 0;
            uint32_t args[2]; int wide[2] = { 1, 1 }, nargs = 2;
            int16_t h = 0, v = 0, val = 0;
            switch (kCases[c].kind) {
            case K_EXPL: args[0] = SRC; args[1] = HANDLES + (uint32_t)rr(0, 3) * 4; break;
            case K_MUSH: h = (int16_t)rr(-2, 28); v = (int16_t)rr(-2, 32); args[0] = (uint16_t)h; args[1] = (uint16_t)v; wide[0] = wide[1] = 0; break;
            case K_FRAG: args[0] = rnd32(); args[1] = rnd32(); break;
            case K_SPARK: args[0] = SRC; nargs = 1; break;
            case K_PSYC: case K_MULT: case K_FODDER: case K_FLEA: case K_HEAD: case K_SHIELD: case K_HOUSE: case K_LAUNCH: case K_ROUND: case K_BUILD: nargs = 0; break;
            case K_SH_FRAME: case K_SH_MOVE: args[0] = SRC; nargs = 1; break;
            default: val = (int16_t)(rr(0, 3) ? rr(0, 99999) % 32768 : rr(-5, 30)); args[0] = SRC; args[1] = (uint32_t)(uint16_t)val << 16; break;
            }
            m68k_call(kCases[c].addr, args, wide, nargs, 200000);
            switch (kCases[c].kind) {
            case K_EXPL: spawn_explosion(&ctx, args[0], args[1]); break;
            case K_MUSH: spawn_mushroom(&ctx, h, v); break;
            case K_FRAG: spawn_fragments(&ctx, args[0], args[1]); break;
            case K_SPARK: spawn_sparks(&ctx, SRC); break;
            case K_BUILD: build_round(&ctx); break;
            case K_ROUND: clear_playfield(&ctx); break;
            case K_SHIELD: shield_start(&ctx); break;
            case K_HOUSE: house_clean(&ctx); break;
            case K_SH_FRAME: frame_shield(&ctx, SRC); break;
            case K_SH_MOVE: move_shield(&ctx, SRC); break;
            case K_LAUNCH: timed_launches(&ctx); break;
            case K_FLEA: spawn_fleas(&ctx); break;
            case K_HEAD: add_pede_head(&ctx); break;
            case K_PSYC: spawn_psychedelic(&ctx); break;
            case K_MULT: spawn_multiplier(&ctx); break;
            case K_FODDER: spawn_fodder(&ctx); break;
            case K_DIG_A: spawn_score_digits(&ctx, SRC, val, 0); break;
            default: spawn_score_digits(&ctx, SRC, val, 1); break;
            }
            if (compare(kCases[c].name, t)) bad++;
        }
        printf("%-30s %d trials, %d mismatches\n", kCases[c].name, trials, bad);
        total_bad += bad;
    }
    return total_bad ? 1 : 0;
}
