/* Differential test: the creature move/frame callbacks of CODE 3 in the original 68k code against src/creatures.c.
 *
 * Each trial builds a random but plausible game state (a sprite, the playfield grid with random mushrooms, the globals around A5, the RNG seed),
 * runs the original routine in Musashi and the C version on a copy of the same state, then compares every byte of the touched memory and
 * the list of calls made to routines that are not ported (sound player, explosion and mushroom allocators), which are stubbed on both sides.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/creatures.h"
#include "harness.h"

#define SPRITE   0x300000u
#define GRID     0x310400u
#define CELLS    0x320000u
#define CELLSZ   0x50u
#define PLAYF    0x330000u
#define RECT     0x330100u
#define PEDEREC  0x340000u
#define SEED     0xA01C2u
#define REC1     0x350000u   /* pede segment record (sprite + 0x52) */
#define AHEAD    0x360000u   /* the segment ahead (a sprite), its record, and a second sprite returned by the overlap search */
#define REC2     0x361000u
#define OTHER    0x370000u
#define OREC     0x371000u
#define GRID2    0x315400u   /* pede occupancy grid */

static uint32_t xs = 0x2545F491u;
static uint32_t rnd32(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }
static int rr(int lo, int hi) { return lo + (int)(rnd32() % (uint32_t)(hi - lo + 1)); }

typedef struct { uint32_t base, len; } Region;
static Region g_regions[16];
static int g_nreg;

static uint8_t *cmem;
static GameCtx ctx;

/* log of calls made by the C version */
static CallRec c_calls[64];
static int c_ncalls;
static uint32_t g_overlap_ret;                  /* what the stubbed overlap search returns, on both sides */
static uint32_t c_ext(void *ud, int kind, const uint8_t *raw, int n)
{
    (void)ud;
    if (c_ncalls >= 64) return 0;
    CallRec *c = &c_calls[c_ncalls++];
    c->addr = kind == EXT_SOUND ? g_a5 + 0x3C2u : kind == EXT_EXPLOSION ? 0x43E26u : kind == EXT_FIND_OVERLAP ? 0x4055Eu : 0x431A0u;
    c->n = n;
    memcpy(c->raw, raw, (size_t)n);
    return kind == EXT_FIND_OVERLAP ? g_overlap_ret : 0;
}

static void put(uint32_t a, uint32_t v, int bytes)
{
    if (bytes == 4) wr32(a, v); else if (bytes == 2) wr16(a, (uint16_t)v); else g_mem[a] = (uint8_t)v;
}
static void g5(int off, uint32_t v, int bytes) { put(g_a5 + (uint32_t)off, v, bytes); }

static uint32_t fixed(int px_lo, int px_hi)
{
    return ((uint32_t)rr(px_lo, px_hi) << 16) | (rnd32() & 0xFFFF);
}

static void setup(int pede)
{
    /* clear the regions that are reused */
    memset(&g_mem[0x30F000], 0, 0x3000);
    memset(&g_mem[CELLS], 0, 0x10000);
    for (uint32_t i = 0; i < 0x100; i++) g_mem[SPRITE + i] = (uint8_t)rnd32();
    for (uint32_t i = 0; i < 0x200; i++) g_mem[PLAYF + i] = 0;
    for (uint32_t i = 0; i < 0x100; i++) g_mem[PEDEREC + i] = (uint8_t)rnd32();

    /* the sprite */
    wr16(SPRITE + 0x18, (uint16_t)rr(8, 40));
    wr16(SPRITE + 0x1A, (uint16_t)rr(8, 40));
    for (int k = 0x1C; k <= 0x1F; k++) g_mem[SPRITE + k] = (uint8_t)(rnd32() & 1);
    wr32(SPRITE + 0x2A, fixed(-30, 450));
    wr32(SPRITE + 0x2E, fixed(-40, 520));
    wr32(SPRITE + 0x3A, rr(0, 4) == 0 ? 0 : fixed(-6, 6));
    wr32(SPRITE + 0x3E, rr(0, 4) == 0 ? 0 : fixed(-6, 8));
    wr16(SPRITE + 0x46, (uint16_t)rr(1, 40));
    wr16(SPRITE + 0x48, (uint16_t)rr(0, 45));
    int m = rr(0, 5);
    if (m == 0) wr32(SPRITE + 0x52, 0x12B);
    else if (m == 1) wr32(SPRITE + 0x52, (uint32_t)rr(0, 1));
    else if (m == 2) wr32(SPRITE + 0x52, PEDEREC);
    if (pede) wr32(SPRITE + 0x52, PEDEREC);
    if (rr(0, 3) == 0) wr32(SPRITE + 0x52, (uint32_t)((int)rd16(SPRITE + 0x48) - rr(0, 10)));   /* spark frame: close to its start frame */
    if (rr(0, 3) == 0) wr32(SPRITE + 0x52, (uint32_t)rr(0, 0x1F));
    wr32(SPRITE + 0, rr(0, 1) ? 0x70626F64u : 0x64656164u);
    g_mem[PEDEREC + 0x84] = (uint8_t)rr(-1, 7);
    wr16(PEDEREC + 0x88, (uint16_t)rr(0, 20));

    /* globals */
    g5(-0x6F0, GRID, 4);
    g5(-0x6DE, rnd32() & 0xFFFFF, 4);
    g5(-0x702, rr(0, 1) ? (uint32_t)rr(0, 4999) : (uint32_t)rr(5000, 90000), 4);
    g5(-0x6A6, rnd32(), 4);
    g5(-0x6BA, rnd32(), 4);
    g5(-0x6BE, (uint32_t)rr(0, 3), 2);
    g5(-0x6E4, (uint32_t)rr(0, 30), 2);
    g5(-0x6A2, (uint32_t)rr(0, 30), 2);
    g5(-0x4B6, (uint32_t)rr(0x1B0, 0x1C0), 2);
    g5(-0x654, (uint32_t)rr(-20, 20), 2);
    g5(-0x610, rnd32(), 4);
    g5(-0x51C, rnd32(), 4); g5(-0x558, rnd32(), 4); g5(-0x554, rnd32(), 4); g5(-0x514, rnd32(), 4);
    g5(-0x548, rnd32(), 4); g5(-0x524, rnd32(), 4);
    g5(-0x758, PLAYF, 4);
    wr32(PLAYF + 0x14, RECT);
    wr16(RECT + 0x14, (uint16_t)rr(0x1D0, 0x1F0));
    wr16(RECT + 0x16, (uint16_t)rr(0x1A0, 0x1C0));
    wr32(SEED, rnd32());

    /* the mushroom grid: 27 x 30 cells of 4-byte pointers */
    int idx = 0;
    int density = rr(5, 60);
    for (int h = 0; h < 27; h++)
        for (int v = 0; v < 30; v++, idx++) {
            if (rr(0, 99) >= density) continue;
            uint32_t rec = CELLS + (uint32_t)idx * CELLSZ;
            for (uint32_t k = 0; k < CELLSZ; k++) g_mem[rec + k] = (uint8_t)rnd32();
            for (int k = 0x1C; k <= 0x1F; k++) g_mem[rec + k] = (uint8_t)rr(0, 2);
            wr16(rec + 0x48, (uint16_t)rr(0, 9));
            wr32(GRID + (uint32_t)h * 0x78 + (uint32_t)v * 4, rec);
        }
}

static void setup_pede(void)
{
    static const uint32_t tags[3] = { 0x70686564u, 0x70626F64u, 0x64656164u };   /* phed, pbod, other */
    int k = rr(0, 9);
    wr32(SPRITE, tags[k < 4 ? 0 : k < 8 ? 1 : 2]);
    wr32(SPRITE + 0x52, REC1);
    memset(&g_mem[0x314000], 0, 0x3000);
    for (uint32_t i = 0; i < 0x100; i++) {
        g_mem[REC1 + i] = (uint8_t)rnd32(); g_mem[AHEAD + i] = (uint8_t)rnd32(); g_mem[REC2 + i] = (uint8_t)rnd32();
        g_mem[OTHER + i] = (uint8_t)rnd32(); g_mem[OREC + i] = (uint8_t)rnd32();
    }
    wr16(REC1, (uint16_t)rr(0, 9)); wr16(REC1 + 2, (uint16_t)rr(0, 9));
    g_mem[REC1 + 0x84] = (uint8_t)(rr(0, 9) < 7 ? rr(1, 4) : rr(-1, 6));
    g_mem[REC1 + 0x85] = (uint8_t)rr(0, 5);
    wr16(REC1 + 0x86, (uint16_t)rr(-20, 480));
    g_mem[REC1 + 0x8A] = (uint8_t)rr(0, 3) == 0;
    wr32(REC1 + 0x7C, AHEAD);
    wr32(REC1 + 0x80, rr(0, 1) ? OTHER : (uint32_t)rr(0, 1));
    wr32(AHEAD + 0x52, REC2);
    wr16(REC2, (uint16_t)rr(0, 9)); wr16(REC2 + 2, (uint16_t)rr(0, 9));
    for (uint32_t e = 0; e < 10; e++) {
        wr32(REC2 + 4 + e * 12, (uint32_t)rr(-30, 460));
        wr32(REC2 + 8 + e * 12, (uint32_t)rr(-40, 520));
        g_mem[REC2 + 0xC + e * 12] = (uint8_t)rr(0, 5); g_mem[REC2 + 0xD + e * 12] = (uint8_t)rr(0, 5);
    }
    wr32(OTHER + 0x52, OREC);
    wr32(OTHER + 0x2A, fixed(-10, 450));
    wr16(OTHER + 0x18, (uint16_t)rr(8, 40));
    g_mem[OREC + 0x84] = (uint8_t)(rr(0, 9) < 8 ? rr(1, 2) : rr(0, 4));
    wr32(SPRITE + 0x2A, fixed(-10, 450));
    wr32(SPRITE + 0x2E, fixed(-10, 500));
    g5(-0x650, (uint32_t)rr(400, 490), 2);
    g5(-0x654, (uint32_t)rr(-5, 40), 2);
    g5(-0x64E, (uint32_t)rr(380, 440), 2);
    g5(-0x652, (uint32_t)rr(-5, 30), 2);
    g5(-0x6E8, (uint32_t)rr(0, 20), 2);
    g5(-0x664, (uint32_t)rr(0, 200), 2);
    g5(-0x6D2, (uint32_t)rr(0, 1), 1);
    g5(-0x6D0, rnd32(), 4);
    g5(-0x648, rnd32(), 4);
    g5(-0x4F8, rnd32(), 4);
    g5(-0x6EC, GRID2, 4);
    /* most grid cells that hold something are mushrooms */
    for (int h = 0; h < 27; h++)
        for (int v = 0; v < 30; v++) {
            uint32_t rec = rd32(GRID + (uint32_t)h * 0x78 + (uint32_t)v * 4);
            if (rec && rr(0, 9)) wr32(rec, 0x7368726Du);
        }
    if (rr(0, 2) == 0) {                         /* targets right at the boundary of the comparisons */
        int vpx = (int)(int32_t)rd32(SPRITE + 0x2E) >> 16, st = (int16_t)rd16(g_a5 - 0x6E8);
        int dir = (int8_t)g_mem[REC1 + 0x84];
        if (dir == 3) wr16(REC1 + 0x86, (uint16_t)(vpx - st + rr(-1, 1)));
        else wr16(REC1 + 0x86, (uint16_t)(vpx + st + rr(-1, 1)));
    }
    static const uint32_t rets[3] = { 0, OTHER, 0 };
    g_overlap_ret = rets[rr(0, 2)];
    if (rr(0, 3) == 0) g_overlap_ret = rd32(REC1 + 0x80);
    harness_set_stub_ret(0x4055Eu, g_overlap_ret);
}

static void snapshot_to_c(void)
{
    for (int i = 0; i < g_nreg; i++) memcpy(&cmem[g_regions[i].base], &g_mem[g_regions[i].base], g_regions[i].len);
}

static int compare(const char *name, long trial, uint32_t arg_dbg)
{
    for (int i = 0; i < g_nreg; i++) {
        const uint8_t *a = &g_mem[g_regions[i].base], *b = &cmem[g_regions[i].base];
        if (memcmp(a, b, g_regions[i].len) == 0) continue;
        for (uint32_t k = 0; k < g_regions[i].len; k++)
            if (a[k] != b[k]) {
                printf("  %s trial %ld: memory differs at %06X (68k %02X, C %02X) arg=%X\n", name, trial, g_regions[i].base + k, a[k], b[k], arg_dbg);
                return 1;
            }
    }
    if (g_ncalls != c_ncalls) {
        printf("  %s trial %ld: %d calls to unported routines in 68k, %d in C\n", name, trial, g_ncalls, c_ncalls);
        return 1;
    }
    for (int i = 0; i < g_ncalls; i++)
        if (g_calls[i].addr != c_calls[i].addr || g_calls[i].n != c_calls[i].n || memcmp(g_calls[i].raw, c_calls[i].raw, (size_t)g_calls[i].n)) {
            printf("  %s trial %ld: call %d differs (68k addr %06X, C addr %06X)\n", name, trial, i, g_calls[i].addr, c_calls[i].addr);
            return 1;
        }
    return 0;
}

typedef void (*CFn)(GameCtx *, uint32_t);
typedef struct { const char *name; uint32_t addr; CFn fn; } Case;

static int16_t t_h, t_v;                       /* arguments of the (h, v) routine */
static void c_remove_mushroom(GameCtx *g, uint32_t s) { (void)s; grid_remove_mushroom(g, t_h, t_v); }

static const Case kCases[] = {
    { "seg3_08DA deactivate",      0x408DA, sprite_deactivate },
    { "seg3_0B0E default move",    0x40B0E, sprite_move_default },
    { "seg3_0BD8 frame (to count)",0x40BD8, frame_next_to_count },
    { "seg3_4260 frame (flea)",    0x44260, frame_next_to_count },
    { "seg3_492E frame (ship)",    0x4492E, frame_next_to_count },
    { "seg3_5554 frame",           0x45554, frame_next_to_count },
    { "seg3_4310 frame (gecko)",   0x44310, frame_gecko },
    { "seg3_4BA6 frame (scorpion)",0x44BA6, frame_scorpion },
    { "seg3_47C2 frame (psychedelic)", 0x447C2, frame_psychedelic },
    { "seg3_51A4 frame (pede body)",   0x451A4, frame_pede_body },
    { "seg3_4286 gecko",           0x44286, move_gecko },
    { "seg3_4330 scorpion",        0x44330, move_scorpion },
    { "seg3_40B4 flea",            0x440B4, move_flea },
    { "seg3_4954 spaceship",       0x44954, move_spaceship },
    { "seg3_4AE6 coin",            0x44AE6, move_coin },
    { "seg3_4832 multiplier",      0x44832, move_timed_bonus },
    { "seg3_47E8 psychedelic",     0x447E8, move_timed_bonus },
    { "seg3_4A9A bullet",          0x44A9A, move_bullet },
    { "seg3_3296 remove mushroom", 0x43296, c_remove_mushroom },
    { "seg3_4BC6 pede head/body",  0x44BC6, move_pede },
    { "seg3_4B6A explosion frame", 0x44B6A, frame_explosion },
    { "seg3_472E score digit move",0x4472E, move_score_digit },
    { "seg3_44EE fragment move",   0x444EE, move_fragment },
    { "seg3_44BA fragment frame",  0x444BA, frame_fragment },
    { "seg3_46C8 spark move",      0x446C8, move_spark },
    { "seg3_469A spark frame",     0x4469A, frame_spark },
};

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../../../apeiron_work";
    int trials = argc > 2 ? atoi(argv[2]) : 100000;
    if (image_load(dir)) return 2;
    cmem = (uint8_t *)calloc(MEM_SIZE, 1);
    ctx.mem = cmem; ctx.a5 = g_a5; ctx.ext = c_ext;

    g_regions[g_nreg++] = (Region){ SPRITE, 0x100 };
    g_regions[g_nreg++] = (Region){ 0x30F000, 0x3000 };
    g_regions[g_nreg++] = (Region){ CELLS, 0x10000 };
    g_regions[g_nreg++] = (Region){ PLAYF, 0x200 };
    g_regions[g_nreg++] = (Region){ PEDEREC, 0x100 };
    g_regions[g_nreg++] = (Region){ A5_WORLD, g_world_len };
    g_regions[g_nreg++] = (Region){ 0xA01B0, 0x20 };
    g_regions[g_nreg++] = (Region){ REC1, 0x100 };
    g_regions[g_nreg++] = (Region){ AHEAD, 0x100 };
    g_regions[g_nreg++] = (Region){ REC2, 0x100 };
    g_regions[g_nreg++] = (Region){ OTHER, 0x100 };
    g_regions[g_nreg++] = (Region){ OREC, 0x100 };
    g_regions[g_nreg++] = (Region){ 0x314000, 0x3000 };

    int total_bad = 0;
    long total = 0;
    for (size_t c = 0; c < sizeof kCases / sizeof kCases[0]; c++) {
        const Case *cs = &kCases[c];
        if (argc > 3 && !strstr(cs->name, argv[3])) continue;
        harness_clear_stubs();
        harness_add_stub(g_a5 + 0x3C2u, 10);
        harness_add_stub(0x43E26u, 8);
        harness_add_stub(0x431A0u, 4);
        harness_add_stub(0x4055Eu, 8);
        int bad = 0;
        for (int t = 0; t < trials && bad < 5; t++) {
            setup(cs->fn == frame_pede_body);
            if (cs->fn == move_pede) setup_pede();
            snapshot_to_c();
            uint32_t seed0 = rd32(SEED);
            c_ncalls = 0;
            uint32_t args[2]; int wide[2]; int nargs;
            if (cs->fn == c_remove_mushroom) {
                t_h = (int16_t)rr(-3, 29); t_v = (int16_t)rr(-3, 32);
                args[0] = (uint16_t)t_h; args[1] = (uint16_t)t_v; wide[0] = wide[1] = 0; nargs = 2;
            } else {
                args[0] = SPRITE; wide[0] = 1; nargs = 1;
            }
            extern int g_trace; g_trace = (getenv("TRACE") && t == atoi(getenv("TT")) && strstr(cs->name, getenv("TRACE")));
            m68k_call(cs->addr, args, wide, nargs, 200000);
            g_trace = 0;
            cs->fn(&ctx, SPRITE);
            if (compare(cs->name, t, args[0])) { bad++; printf("    seed0=%08X 68k seed=%08X C seed=%08X\n", seed0, rd32(SEED), gm_rd32(&ctx, SEED)); }
            total++;
        }
        printf("%-34s %d trials, %d mismatches\n", cs->name, trials, bad);
        total_bad += bad;
    }
    printf("TOTAL %ld trials, %d mismatching functions-trials\n", total, total_bad);
    return total_bad ? 1 : 0;
}
