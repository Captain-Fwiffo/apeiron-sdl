/* Differential test of the shooter, its shots, coins and the shot hit handler (CODE 3) against src/creatures.c:
 *   seg3_5216 shooter move, 54C8 shooter frame, 54DA shot move, 454C coin move, 2256 fire, 3968 root, 3EF4 coins, 557A shot hit.
 * The hit handler walks lists of random pede segments and creatures placed near the shot; the sound player, the special-mushroom routine (4010)
 * and the pause hook are stubbed on both sides and compared by arguments.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/creatures.h"
#include "harness.h"

#define ENGINE   0x3A0000u
#define LISTS    0x3A2000u           /* effects, mushrooms, pede, extras, shots: 0x20 each */
#define GRID     0x310400u
#define GRID2    0x315400u
#define CELLS    0x320000u
#define HANDLES  0x3C0000u
#define CLASSDATA 0x3C1000u
#define RECS     0x350000u
#define SPRITES  0x3E0000u
#define SHOT     0x3E0000u
#define PLAYER   0x3E0100u
#define HEAP     0x500000u
#define SEED     0xA01C2u

static uint32_t xs = 0x51ED270Bu;
static uint32_t rnd32(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }
static int rr(int lo, int hi) { return lo + (int)(rnd32() % (uint32_t)(hi - lo + 1)); }

typedef struct { uint32_t base, len; } Region;
static Region g_regions[16];
static int g_nreg;
static uint8_t *cmem;
static GameCtx ctx;

static CallRec c_calls[32];
static int c_ncalls;
static uint32_t c_ext(void *ud, int kind, const uint8_t *raw, int n)
{
    (void)ud;
    if (c_ncalls >= 32) return 0;
    CallRec *c = &c_calls[c_ncalls++];
    c->addr = kind >= 0x1000 ? (uint32_t)kind : kind == EXT_SOUND ? g_a5 + 0x3C2u : 0;
    c->n = n;
    if (n) memcpy(c->raw, raw, (size_t)n);
    return 0;
}

static void g5(int off, uint32_t v, int bytes)
{
    uint32_t a = g_a5 + (uint32_t)off;
    if (bytes == 4) wr32(a, v); else if (bytes == 2) wr16(a, (uint16_t)v); else g_mem[a] = (uint8_t)v;
}
static uint32_t fixed(int lo, int hi) { return ((uint32_t)rr(lo, hi) << 16) | (rnd32() & 0xFFFF); }

static void build_class(int k)
{
    uint32_t h = HANDLES + (uint32_t)k * 4, d = CLASSDATA + (uint32_t)k * 0x400;
    wr32(h, d);
    for (uint32_t i = 0; i < 0x400; i += 4) wr32(d + i, rnd32());
    wr16(d + 4, (uint16_t)rr(3, 8));
    wr16(d + 6, (uint16_t)rr(0, 3)); wr16(d + 8, (uint16_t)rr(0, 3));
    wr16(d + 0xA, (uint16_t)rr(8, 24)); wr16(d + 0xC, (uint16_t)rr(8, 24));
}

static uint32_t nspr;
static uint32_t nrec;

static uint32_t add_sprite(uint32_t list, uint32_t tag, int near_h, int near_v)
{
    uint32_t s = SPRITES + 0x200 + nspr++ * 0x100;
    for (uint32_t i = 0; i < 0x100; i += 4) wr32(s + i, rnd32());
    wr32(s, tag);
    wr16(s + 0x18, 16); wr16(s + 0x1A, 16);
    int16_t top = (int16_t)(near_v + rr(-10, 10)), left = (int16_t)(near_h + rr(-10, 10));
    if (rr(0, 3) == 0) { top = (int16_t)rr(0, 480); left = (int16_t)rr(0, 430); }
    wr16(s + 8, (uint16_t)top); wr16(s + 0xA, (uint16_t)left); wr16(s + 0xC, (uint16_t)(top + 16)); wr16(s + 0xE, (uint16_t)(left + 16));
    wr32(s + 0x2A, ((uint32_t)(uint16_t)left << 16) | (rnd32() & 0xFFFF));
    wr32(s + 0x2E, ((uint32_t)(uint16_t)top << 16) | (rnd32() & 0xFFFF));
    g_mem[s + 0x1C] = rr(0, 5) ? 1 : 0; g_mem[s + 0x1D] = 0; g_mem[s + 0x1E] = 0; g_mem[s + 0x1F] = 0;
    wr16(s + 0x48, (uint16_t)rr(0, 10)); wr16(s + 0x46, (uint16_t)rr(2, 8));
    wr32(s + 0x52, (uint32_t)rr(0, 3) | ((uint32_t)rr(0, 2) << 16));
    wr32(s + 0x3A, rr(0, 1) ? 0 : fixed(-3, 3));
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
    memset(&g_mem[LISTS], 0, 0x100);
    memset(&g_mem[0x30F000], 0, 0x3000);
    memset(&g_mem[0x314000], 0, 0x3000);
    memset(&g_mem[CELLS], 0, 0x10000);
    memset(&g_mem[SPRITES], 0, 0x4000);
    memset(&g_mem[RECS], 0, 0x2000);
    memset(&g_mem[HEAP], 0, 0x8000);
    for (int k = 0; k < 8; k++) build_class(k);
    g_heap_next = HEAP; ctx.heap_next = HEAP;
    nspr = 0; nrec = 0;
    g5(-0x75A, 1, 1); g5(-0x758, ENGINE, 4);
    wr16(ENGINE + 0x2A, (uint16_t)rr(0, 5));
    uint32_t le = LISTS, lm = LISTS + 0x20, lp = LISTS + 0x40, lx = LISTS + 0x60, ls = LISTS + 0x80;
    for (int i = 0; i < 5; i++) { uint32_t l = LISTS + (uint32_t)i * 0x20; wr32(l, rnd32()); g_mem[l + 0xC] = 1; }
    g5(-0x63C, le, 4); g5(-0x64C, lm, 4); g5(-0x648, lp, 4); g5(-0x640, lx, 4); g5(-0x644, ls, 4);
    /* classes */
    static const int cls_off[] = { -0x638, -0x62C, -0x61C, -0x60C, -0x618, -0x614, -0x610, -0x608, -0x604, -0x600, -0x5FC, -0x5E8, -0x5F4 };
    for (size_t i = 0; i < sizeof cls_off / sizeof cls_off[0]; i++) g5(cls_off[i], HANDLES + (uint32_t)rr(0, 7) * 4, 4);
    /* sounds */
    static const int snd[] = { -0x580, -0x544, -0x530, -0x564, -0x57C, -0x574, -0x570, -0x540, -0x510, -0x504, -0x528, -0x514, -0x52C, -0x50C,
                               -0x55C, -0x550, -0x500, -0x584, -0x548 };
    for (size_t i = 0; i < sizeof snd / sizeof snd[0]; i++) g5(snd[i], rnd32(), 4);
    /* globals */
    uint32_t ticks = rnd32() & 0xFFFFF;
    wr32(0x16A, ticks);
    g5(-0x6DE, ticks, 4);
    g5(-0x6F0, GRID, 4); g5(-0x6EC, GRID2, 4);
    g5(-0x6E2, (uint32_t)rr(0, 9), 2); g5(-0x6E4, (uint32_t)rr(0, 30), 2); g5(-0x6E6, (uint32_t)rr(0, 12), 2);
    { static const int lv[] = { 11, 12, 22, 23, 0, 49, 50 }; g5(-0x734, (uint32_t)(rr(0, 2) ? rr(0, 30) : lv[rr(0, 6)]), 2); } g5(-0x704, (uint32_t)(rr(0, 3) ? 1 : 10), 2);
    g5(-0x706, (uint32_t)(rr(0, 3) ? 1 : 10), 4); g5(-0x702, rnd32() & 0xFFFFF, 4);
    g5(-0x6CC, (uint32_t)rr(0, 4), 2); g5(-0x6CA, (uint32_t)rr(1, 5), 2);
    g5(-0x6C8, ticks - (uint32_t)rr(0, 10), 4); g5(-0x6C4, (uint32_t)rr(0, 6), 4);
    g5(-0x418, (uint32_t)rr(0, 1), 1); g5(-0x66B, (uint32_t)rr(0, 1), 1); g5(-0x66D, (uint32_t)rr(0, 1), 1); g5(-0x66C, (uint32_t)rr(0, 1), 1);
    g5(-0x654, (uint32_t)rr(0, 40), 2);
    g5(-0x6AA, rnd32(), 2); g5(-0x66E, 0, 1); g5(-0x6C0, (uint32_t)rr(1, 4) * 200, 2);
    g5(-0x6BE, (uint32_t)rr(0, 3), 2); g5(-0x6BA, rnd32(), 4); g5(-0x682, rnd32() & 0xFFFF, 4);
    g5(-0x714, rnd32(), 4); g5(-0x710, rnd32(), 4); g5(-0x716, 0, 1); g5(-0x708, (uint32_t)rr(0, 1), 1);
    g5(-0x6A6, rnd32(), 4); g5(-0x41C, rnd32(), 2); g5(-0x41A, rnd32(), 2);
    g5(-0x674, (uint32_t)rr(1, 5), 2);
    g5(-0x65E, (uint32_t)rr(380, 420), 2); g5(-0x662, (uint32_t)rr(0, 20), 2);
    g5(-0x660, (uint32_t)rr(440, 480), 2); g5(-0x664, (uint32_t)rr(340, 400), 2);
    wr32(SEED, rnd32());

    /* the player (also the shooter being moved) */
    for (uint32_t i = 0; i < 0x100; i += 4) wr32(PLAYER + i, rnd32());
    wr32(PLAYER + 0x2A, fixed(0, 420)); wr32(PLAYER + 0x2E, fixed(330, 470));
    wr16(PLAYER + 0x8, (uint16_t)rr(300, 470)); wr16(PLAYER + 0x46, (uint16_t)rr(3, 9)); wr16(PLAYER + 0x48, (uint16_t)rr(1, 9));
    g5(-0x59C, PLAYER, 4);
    /* the shot */
    for (uint32_t i = 0; i < 0x100; i += 4) wr32(SHOT + i, rnd32());
    int32_t sh = rr(0, 420), sv = rr(0, 470);
    wr32(SHOT + 0x2A, ((uint32_t)sh << 16) | (rnd32() & 0xFFFF)); wr32(SHOT + 0x2E, ((uint32_t)sv << 16) | (rnd32() & 0xFFFF));
    wr16(SHOT + 8, (uint16_t)sv); wr16(SHOT + 0xA, (uint16_t)sh); wr16(SHOT + 0xC, (uint16_t)(sv + 16)); wr16(SHOT + 0xE, (uint16_t)(sh + 16));
    g_mem[SHOT + 0x1C] = rr(0, 7) ? 1 : 0;
    wr32(SHOT + 0x36, rr(0, 1) ? 0 : (uint32_t)rr(1, 1000000));
    /* the mushroom grid */
    int idx = 0, density = rr(0, 60);
    for (int h = 0; h < 27; h++)
        for (int v = 0; v < 30; v++, idx++) {
            if (rr(0, 99) >= density) continue;
            uint32_t rec = CELLS + (uint32_t)idx * 0x50;
            for (uint32_t k = 0; k < 0x50; k += 4) wr32(rec + k, rnd32());
            wr32(rec, 0x7368726Du);
            wr16(rec + 0x48, (uint16_t)rr(0, 16)); wr16(rec + 0x46, (uint16_t)rr(2, 8));
            g_mem[rec + 0x1C] = rr(0, 5) ? 1 : 0;
            wr32(GRID + (uint32_t)h * 0x78 + (uint32_t)v * 4, rec);
        }
    /* pede segments (a chain, sometimes several) near the shot */
    int np = rr(0, 4);
    uint32_t prev = 0;
    for (int i = 0; i < np; i++) {
        uint32_t s = add_sprite(lp, rr(0, 3) ? 0x70626F64u : 0x70686564u, sh, sv);
        uint32_t rec = RECS + nrec++ * 0x100;
        for (uint32_t k = 0; k < 0x100; k += 4) wr32(rec + k, rnd32());
        wr32(s + 0x52, rec);
        g_mem[rec + 0x8A] = rr(0, 1); g_mem[rec + 0x84] = (uint8_t)rr(0, 5); g_mem[rec + 0x85] = (uint8_t)rr(0, 5);
        wr32(rec + 0x7C, prev); wr32(rec + 0x80, 0);
        if (prev) wr32(rd32(prev + 0x52) + 0x80, s);
        prev = s;
        wr32(GRID2 + (uint32_t)rr(0, 26) * 0x78 + (uint32_t)rr(0, 29) * 4, s);
    }
    /* creatures near the shot */
    static const uint32_t tags[] = { 0x666C6561u, 0x6D756C74u, 0x6765636Bu, 0x70737963u, 0x79756D79u, 0x666F6472u, 0x726F6F74u, 0x73636F72u, 0x6F746865u };
    int nx = rr(0, 3);
    for (int i = 0; i < nx; i++) add_sprite(lx, tags[rr(0, 8)], sh, sv);
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
    if (g_ncalls != c_ncalls) {
        printf("  %s trial %ld: %d logged calls in 68k, %d in C\n", name, trial, g_ncalls, c_ncalls);
        if (getenv("SHOW")) for (int i = 0; i < g_ncalls || i < c_ncalls; i++) printf("     %2d  68k %06X   C %06X\n", i, i < g_ncalls ? g_calls[i].addr : 0, i < c_ncalls ? c_calls[i].addr : 0);
        return 1;
    }
    for (int i = 0; i < g_ncalls; i++)
        if (g_calls[i].addr != c_calls[i].addr || g_calls[i].n != c_calls[i].n || memcmp(g_calls[i].raw, c_calls[i].raw, (size_t)g_calls[i].n)) {
            printf("  %s trial %ld: call %d differs (68k %06X, C %06X)\n", name, trial, i, g_calls[i].addr, c_calls[i].addr);
            return 1;
        }
    return 0;
}

enum { K_SHOOTER, K_SHOOTER_FRAME, K_SHOT_MOVE, K_YUMMY, K_FIRE, K_ROOT, K_COINS, K_HIT, K_SPECIAL, K_MARK };
static const struct { const char *name; uint32_t addr; int kind; } kCases[] = {
    { "seg3_5216 shooter move", 0x45216, K_SHOOTER }, { "seg3_54C8 shooter frame", 0x454C8, K_SHOOTER_FRAME },
    { "seg3_54DA shot move", 0x454DA, K_SHOT_MOVE }, { "seg3_454C coin move", 0x4454C, K_YUMMY }, { "seg3_2256 fire", 0x42256, K_FIRE },
    { "seg3_3968 root", 0x43968, K_ROOT }, { "seg3_3EF4 coins", 0x43EF4, K_COINS }, { "seg3_557A shot hit", 0x4557A, K_HIT }, { "seg3_4010 special shot", 0x44010, K_SPECIAL }, { "seg3_0F2C mark mushrooms", 0x40F2C, K_MARK } };

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../../../apeiron_work";
    int trials = argc > 2 ? atoi(argv[2]) : 50000;
    if (image_load(dir)) return 2;
    cmem = (uint8_t *)calloc(MEM_SIZE, 1);
    ctx.mem = cmem; ctx.a5 = g_a5; ctx.ext = c_ext;
    g_regions[g_nreg++] = (Region){ A5_WORLD, g_world_len };
    g_regions[g_nreg++] = (Region){ ENGINE, 0x100 };
    g_regions[g_nreg++] = (Region){ LISTS, 0x100 };
    g_regions[g_nreg++] = (Region){ 0x30F000, 0x3000 };
    g_regions[g_nreg++] = (Region){ 0x314000, 0x3000 };
    g_regions[g_nreg++] = (Region){ CELLS, 0x10000 };
    g_regions[g_nreg++] = (Region){ HANDLES, 0x4000 };
    g_regions[g_nreg++] = (Region){ RECS, 0x2000 };
    g_regions[g_nreg++] = (Region){ SPRITES, 0x4000 };
    g_regions[g_nreg++] = (Region){ HEAP, 0x8000 };
    g_regions[g_nreg++] = (Region){ 0xA01B0, 0x20 };
    int total_bad = 0;
    for (size_t c = 0; c < sizeof kCases / sizeof kCases[0]; c++) {
        if (argc > 3 && !strstr(kCases[c].name, argv[3])) continue;
        harness_clear_stubs();
        harness_add_stub(g_a5 + 0x3C2u, 10); harness_add_stub(g_a5 + 0x3DAu, 2); 
        int bad = 0;
        for (int t = 0; t < trials && bad < 5; t++) {
            setup();
            snapshot_to_c();
            c_ncalls = 0;
            uint32_t args[2]; int wide[2] = { 1, 1 }, nargs = 1;
            int consume = rr(0, 4) != 0;
            switch (kCases[c].kind) {
            case K_FIRE: nargs = 0; break;
            case K_COINS: args[0] = PLAYER; args[1] = HANDLES + (uint32_t)rr(0, 7) * 4; nargs = 2; break;
            case K_HIT: args[0] = SHOT; args[1] = consume ? 0x0100u | (rnd32() & 0xFF) : (rnd32() & 0xFF); wide[1] = 0; nargs = 2; break;
            case K_SHOT_MOVE: case K_SPECIAL: args[0] = SHOT; break;
            case K_MARK: args[0] = LISTS + (uint32_t)rr(0, 4) * 0x20; break;
            default: args[0] = (kCases[c].kind == K_SHOOTER || kCases[c].kind == K_SHOOTER_FRAME) ? PLAYER : (kCases[c].kind == K_YUMMY ? SHOT : PLAYER); break;
            }
            m68k_call(kCases[c].addr, args, wide, nargs, 400000);
            switch (kCases[c].kind) {
            case K_SHOOTER: move_shooter(&ctx, PLAYER); break;
            case K_SHOOTER_FRAME: frame_shooter(&ctx, PLAYER); break;
            case K_SHOT_MOVE: move_shot(&ctx, SHOT); break;
            case K_YUMMY: move_yummy(&ctx, SHOT); break;
            case K_FIRE: fire_shot(&ctx); break;
            case K_ROOT: spawn_root(&ctx, PLAYER); break;
            case K_COINS: spawn_coins(&ctx, args[0], args[1]); break;
            case K_HIT: shot_hit(&ctx, SHOT, consume); break;
            case K_SPECIAL: spawn_special_shot(&ctx, SHOT); break;
            case K_MARK: mark_mushrooms_under(&ctx, args[0]); break;
            }
            if (compare(kCases[c].name, t)) bad++;
        }
        printf("%-26s %d trials, %d mismatches\n", kCases[c].name, trials, bad);
        total_bad += bad;
    }
    return total_bad ? 1 : 0;
}
