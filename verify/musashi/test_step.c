/* Differential test: the per-step update (seg3_1BB6) and the score/lives/bonus update (seg3_1A72) of CODE 3 against src/creatures.c.
 *
 * Every routine they call that is not ported yet (sound, drawing, spawning, collision search, key test, Toolbox traps) is stubbed on both
 * sides and compared by address and raw stack arguments; the stubbed collision search and key test return values chosen per trial.
 * The long multiply/divide library calls and the random number generator run for real in the 68k code.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/creatures.h"
#include "harness.h"

#define SEED     0xA01C2u
#define OTHER    0x370000u
#define PLAYER   0x398000u
#define HANDLE1  0x390000u
#define BLOCK1   0x391000u
#define HANDLE2  0x390010u
#define BLOCK2   0x391100u
#define LISTA    0x12345678u
#define LISTB    0x23456789u

static uint32_t xs = 0x9E3779B9u;
static uint32_t rnd32(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }
static int rr(int lo, int hi) { return lo + (int)(rnd32() % (uint32_t)(hi - lo + 1)); }

typedef struct { uint32_t base, len; } Region;
static Region g_regions[16];
static int g_nreg;
static uint8_t *cmem;
static GameCtx ctx;

static uint32_t g_retA, g_retB, g_keyret;
static uint32_t stub_ret(uint32_t addr, const uint8_t *raw)
{
    if (addr == 0x4055Eu) {
        uint32_t list = (uint32_t)raw[4] << 24 | raw[5] << 16 | raw[6] << 8 | raw[7];
        return list == LISTA ? g_retA : list == LISTB ? g_retB : 0;
    }
    if (addr == 0x43348u) return g_keyret;
    return 0;
}

static CallRec c_calls[64];
static int c_ncalls;
static uint32_t c_ext(void *ud, int kind, const uint8_t *raw, int n)
{
    (void)ud;
    if (c_ncalls >= 64) return 0;
    CallRec *c = &c_calls[c_ncalls++];
    c->addr = kind >= 0x1000 ? (uint32_t)kind : kind == EXT_SOUND ? g_a5 + 0x3C2u : kind == EXT_EXPLOSION ? 0x43E26u : kind == EXT_FIND_OVERLAP ? 0x4055Eu : 0x431A0u;
    c->n = n;
    if (n) memcpy(c->raw, raw, (size_t)n);
    return stub_ret(c->addr, c->raw);
}

static void g5(int off, uint32_t v, int bytes)
{
    uint32_t a = g_a5 + (uint32_t)off;
    if (bytes == 4) wr32(a, v); else if (bytes == 2) wr16(a, (uint16_t)v); else g_mem[a] = (uint8_t)v;
}
static uint32_t near_tick(uint32_t ticks, int lo, int hi) { return ticks + (uint32_t)rr(lo, hi); }

static void setup(void)
{
    for (uint32_t i = 0; i < 0x100; i++) { g_mem[PLAYER + i] = (uint8_t)rnd32(); g_mem[OTHER + i] = (uint8_t)rnd32(); }
    for (uint32_t i = 0; i < 0x40; i++) g_mem[HANDLE1 + i] = 0;
    for (uint32_t i = 0; i < 0x20; i++) { g_mem[BLOCK1 + i] = (uint8_t)rnd32(); g_mem[BLOCK2 + i] = (uint8_t)rnd32(); }
    g_mem[PLAYER + 0x1C] = (uint8_t)(rnd32() & 1);
    g_mem[OTHER + 0x1C] = (uint8_t)(rnd32() & 1);
    static const uint32_t tags[4] = { 0x79756D79u, 0x70737963u, 0x64656164u, 0x79756D79u };   /* yumy, psyc, other */
    wr32(OTHER, tags[rr(0, 3)]);
    wr32(HANDLE1, BLOCK1); wr32(HANDLE2, BLOCK2);

    uint32_t ticks = rnd32() & 0xFFFFF;
    int mode = rr(0, 5);
    int32_t score = mode == 0 ? rr(0, 100) : mode == 1 ? rr(0, 20000) : mode == 2 ? rr(0, 200000) : mode == 3 ? rr(0, 3000000) : rr(0, 1500);
    if (mode == 5) { static const int th[] = { 0x13880, 0x186A0, 0x1D4C0, 0x222E0, 0x27100, 0x2BF20, 0x4E20 }; score = th[rr(0, 6)] + rr(-1, 1); }
    g5(-0x6DE, ticks, 4);
    g5(-0x715, (uint32_t)rr(0, 1), 1);
    g5(-0x70C, near_tick(ticks, -40, 0), 4);
    g5(-0x4C4, HANDLE1, 4); g5(-0x71C, HANDLE2, 4);
    g5(-0x72E, rnd32(), 4);
    g5(-0x6D2, (uint32_t)rr(0, 1), 1);
    g5(-0x686, rr(0, 2) ? 0 : near_tick(ticks, -3, 3), 4);
    g5(-0x6AE, rr(0, 2) ? 0 : near_tick(ticks, -3, 3), 4);
    g5(-0x6B2, rr(0, 2) ? 0 : near_tick(ticks, -3, 3), 4);
    g5(-0x702, (uint32_t)score, 4);
    g5(-0x734, (uint32_t)rr(0, 200), 2);
    g5(-0x732, rr(0, 1) ? (uint32_t)rr(0, 200) : 0, 2);
    if (rr(0, 3) == 0) g5(-0x732, rd16(g_a5 - 0x734), 2);
    g5(-0x6A0, rnd32(), 4);
    g5(-0x6F6, rr(0, 3) == 0, 1);
    g5(-0x66A, rr(0, 3) == 0, 1);
    g5(-0x708, (uint32_t)rr(0, 1), 1);
    g5(-0x6F4, near_tick(ticks, -300, 0), 4);
    g5(-0x669, (uint32_t)rr(0, 1), 1); g5(-0x66D, (uint32_t)rr(0, 1), 1); g5(-0x66C, (uint32_t)rr(0, 1), 1);
    g5(-0x66B, (uint32_t)rr(0, 1), 1); g5(-0x66E, (uint32_t)rr(0, 1), 1);
    g5(-0x6C4, rnd32(), 4); g5(-0x6CA, rnd32(), 2);
    g5(-0x6FA, (uint32_t)rr(-1, 9), 2);
    g5(-0x6F8, rr(0, 1) ? rd16(g_a5 - 0x6FA) : (uint32_t)rr(-1, 9), 2);
    g5(-0x730, (uint32_t)rr(0, 1), 1);
    g5(-0x6FE, rr(0, 3) == 0 ? (uint32_t)score : (uint32_t)(score - rr(0, 5000)), 4);
    g5(-0x696, rr(0, 2) ? near_tick(ticks, -5, -1) : ticks, 4);
    g5(-0x69C, rnd32(), 4);
    g5(-0x68E, (uint32_t)(score - rr(0, 45000)), 4);
    g5(-0x6A8, (uint32_t)rr(0, 1), 1);
    g5(-0x67C, near_tick(ticks, -10, 10), 4);
    g5(-0x682, (uint32_t)rr(-5, 300), 4);
    g5(-0x67E, rr(0, 1) ? rd16(g_a5 - 0x680) : rnd32(), 2);
    g5(-0x680, rnd32(), 2);
    g5(-0x678, rnd32(), 4);
    g5(-0x59C, PLAYER, 4);
    g5(-0x648, LISTA, 4); g5(-0x640, LISTB, 4);
    g5(-0x610, rnd32(), 4);
    static const int snd[] = { -0x538, -0x534, -0x518, -0x568, -0x548, -0x560, -0x56C };
    for (int i = 0; i < 7; i++) g5(snd[i], rnd32(), 4);
    wr32(SEED, rnd32());

    g_retA = rr(0, 2) == 0 ? OTHER : 0;
    g_retB = rr(0, 2) ? OTHER : 0;
    g_keyret = (uint32_t)(rr(0, 1) ? 0xFFu : 0u) | (rnd32() & 0xFFFFFF00u);
}

static void snapshot_to_c(void)
{
    for (int i = 0; i < g_nreg; i++) memcpy(&cmem[g_regions[i].base], &g_mem[g_regions[i].base], g_regions[i].len);
}

static int compare(const char *name, long trial)
{
    for (int i = 0; i < g_nreg; i++) {
        const uint8_t *a = &g_mem[g_regions[i].base], *b = &cmem[g_regions[i].base];
        if (memcmp(a, b, g_regions[i].len) == 0) continue;
        for (uint32_t k = 0; k < g_regions[i].len; k++)
            if (a[k] != b[k]) { printf("  %s trial %ld: memory differs at %06X (68k %02X, C %02X)\n", name, trial, g_regions[i].base + k, a[k], b[k]); return 1; }
    }
    if (g_ncalls != c_ncalls) {
        printf("  %s trial %ld: %d stubbed calls in 68k, %d in C\n", name, trial, g_ncalls, c_ncalls);
        if (getenv("SHOW"))
            for (int i = 0; i < g_ncalls || i < c_ncalls; i++)
                printf("     %2d  68k %06X   C %06X\n", i, i < g_ncalls ? g_calls[i].addr : 0, i < c_ncalls ? c_calls[i].addr : 0);
        return 1;
    }
    for (int i = 0; i < g_ncalls; i++)
        if (g_calls[i].addr != c_calls[i].addr || g_calls[i].n != c_calls[i].n || memcmp(g_calls[i].raw, c_calls[i].raw, (size_t)g_calls[i].n)) {
            printf("  %s trial %ld: call %d differs (68k %06X, C %06X)\n", name, trial, i, g_calls[i].addr, c_calls[i].addr);
            return 1;
        }
    return 0;
}

static const struct { uint32_t addr; int n; } kStubs[] = {
    { 0x4055E, 8 }, { 0x43E26, 8 }, { 0x415E4, 9 }, { 0x41428, 0 }, { 0x4157A, 0 }, { 0x42128, 0 }, { 0x43A36, 0 }, { 0x43B02, 0 },
    { 0x43BD8, 0 }, { 0x42256, 0 }, { 0x42334, 0 }, { 0x43348, 0 }, { 0x42596, 0 }, { 0x4363A, 4 }, { 0x426C2, 2 }, { 0x40902, 4 },
    { 0x4299A, 0 }, { 0x400EA, 0 }, { 0x40098, 0 }, { 0x405CE, 0 }, { 0x400C2, 0 }, { 0x42048, 0 }, { 0x420CC, 0 } };

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../../../apeiron_work";
    int trials = argc > 2 ? atoi(argv[2]) : 100000;
    if (image_load(dir)) return 2;
    cmem = (uint8_t *)calloc(MEM_SIZE, 1);
    ctx.mem = cmem; ctx.a5 = g_a5; ctx.ext = c_ext;
    g_stub_cb = stub_ret;
    g_regions[g_nreg++] = (Region){ A5_WORLD, g_world_len };
    g_regions[g_nreg++] = (Region){ 0xA01B0, 0x20 };
    g_regions[g_nreg++] = (Region){ PLAYER, 0x100 };
    g_regions[g_nreg++] = (Region){ OTHER, 0x100 };
    g_regions[g_nreg++] = (Region){ 0x390000, 0x40 };
    g_regions[g_nreg++] = (Region){ BLOCK1, 0x20 };
    g_regions[g_nreg++] = (Region){ BLOCK2 - 0x100 + 0x100, 0x20 };

    static const struct { const char *name; uint32_t addr; int step; } cases[] = {
        { "seg3_1A72 score/lives/bonus update", 0x41A72, 0 }, { "seg3_1BB6 per-step update", 0x41BB6, 1 } };
    int total_bad = 0;
    for (int c = 0; c < 2; c++) {
        harness_clear_stubs();
        harness_add_stub(g_a5 + 0x3C2u, 10); harness_add_stub(g_a5 + 0x4E2u, 4); harness_add_stub(g_a5 + 0x3DAu, 2);
        for (size_t i = 0; i < sizeof kStubs / sizeof kStubs[0]; i++) harness_add_stub(kStubs[i].addr, kStubs[i].n);
        int bad = 0;
        for (int t = 0; t < trials && bad < 5; t++) {
            setup();
            snapshot_to_c();
            c_ncalls = 0;
            { extern int g_trace; g_trace = getenv("TT") && t == atoi(getenv("TT")) && cases[c].step; }
            uint32_t d0 = m68k_call(cases[c].addr, NULL, NULL, 0, 200000);
            uint16_t cret = 0;
            if (cases[c].step) cret = step_update(&ctx); else update_score_display(&ctx);
            if (compare(cases[c].name, t)) bad++;
            else if (cases[c].step && (uint16_t)d0 != cret) { printf("  %s trial %d: returns %04X vs %04X\n", cases[c].name, t, (uint16_t)d0, cret); bad++; }
        }
        printf("%-36s %d trials, %d mismatches\n", cases[c].name, trials, bad);
        total_bad += bad;
    }
    return total_bad ? 1 : 0;
}
