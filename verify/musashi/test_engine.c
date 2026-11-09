/* Differential test of the sprite engine (CODE 3 "Animation Tool") against src/creatures.c:
 *   list creation (0712), sprite allocation (0782), unlink (0884), activate (0902), overlap search (055E),
 *   the move pass (00EA) and the erase/draw pass (020C..).
 * The routines generated at run time that do the drawing are replaced by stubs that log their registers (A0, A1, D0, D1), so the order and the
 * arguments of every blit are compared; _NewPtr is a bump allocator on both sides and _DisposPtr is logged.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/creatures.h"
#include "harness.h"

#define ENGINE   0x3A0000u
#define BOUNDS   0x3A1000u
#define LISTS    0x3A2000u     /* list records, 0x20 apart */
#define ROWS_SAVED  0x3B1000u
#define ROWS_WORK   0x3B3000u
#define ROWS_SCREEN 0x3B5000u
#define HANDLES  0x3C0000u
#define CLASSDATA 0x3C1000u    /* 0x400 each */
#define SPRITES  0x3E0000u     /* 0x100 each */
#define HEAP     0x500000u
#define STUB_B   0x600000u
#define STUB_C   0x610000u
#define NCLASS   3

static uint32_t xs = 0x1234ABCDu;
static uint32_t rnd32(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }
static int rr(int lo, int hi) { return lo + (int)(rnd32() % (uint32_t)(hi - lo + 1)); }

typedef struct { uint32_t base, len; } Region;
static Region g_regions[16];
static int g_nreg;
static uint8_t *cmem;
static GameCtx ctx;

static CallRec c_calls[160];
static int c_ncalls;
static uint32_t c_ext(void *ud, int kind, const uint8_t *raw, int n)
{
    (void)ud;
    if (c_ncalls >= 160) return 0;
    CallRec *c = &c_calls[c_ncalls++];
    c->addr = kind >= 0x1000 ? (uint32_t)kind : kind == EXT_SOUND ? g_a5 + 0x3C2u : 0;
    c->n = n;
    if (n) memcpy(c->raw, raw, (size_t)n);
    return 0;
}

static void w8(uint32_t a, uint32_t v) { g_mem[a] = (uint8_t)v; }
static void g5(int off, uint32_t v, int bytes)
{
    uint32_t a = g_a5 + (uint32_t)off;
    if (bytes == 4) wr32(a, v); else if (bytes == 2) wr16(a, (uint16_t)v); else g_mem[a] = (uint8_t)v;
}
static uint32_t fixed(int lo, int hi) { return ((uint32_t)rr(lo, hi) << 16) | (rnd32() & 0xFFFF); }

static const uint32_t kMoveCbs[] = { 0x40B0E, 0x44AE6, 0x44A9A, 0x44832, 0x447E8, 0x440A4 };
static const uint32_t kFrameCbs[] = { 0x40BD8, 0x44310, 0x44BA6, 0x447C2, 0x440AC, 0x44260 };

static uint32_t nlists, nsprites;
static uint32_t list_addr[8];

static void build_class(int k)
{
    uint32_t h = HANDLES + (uint32_t)k * 4, d = CLASSDATA + (uint32_t)k * 0x400;
    wr32(h, d);
    for (uint32_t i = 0; i < 0x400; i += 4) wr32(d + i, rnd32());
    wr16(d + 4, (uint16_t)rr(3, 8));
    wr16(d + 6, (uint16_t)rr(0, 3)); wr16(d + 8, (uint16_t)rr(0, 3));          /* top, left */
    wr16(d + 0xA, (uint16_t)rr(8, 40)); wr16(d + 0xC, (uint16_t)rr(8, 40));    /* bottom, right */
    wr32(d + 0xE, 0x100); wr32(d + 0x12, 0x200);
    for (int f = 0; f < 8; f++) {
        wr32(d + 0x100 + (uint32_t)f * 4, STUB_B + (uint32_t)k * 0x100 + (uint32_t)f * 8 - d);
        wr32(d + 0x200 + (uint32_t)f * 4, STUB_C + (uint32_t)k * 0x100 + (uint32_t)f * 8 - d);
    }
}

static void build_sprite(uint32_t s, uint32_t list)
{
    for (uint32_t i = 0; i < 0x100; i += 4) wr32(s + i, rnd32());
    int k = rr(0, NCLASS - 1);
    uint32_t now = rd32(0x16A);
    wr32(s + 0, rnd32());
    wr32(s + 4, HANDLES + (uint32_t)k * 4);
    int16_t top = (int16_t)rr(-20, 500), left = (int16_t)rr(-20, 440);
    int16_t h = (int16_t)rr(8, 40), w = (int16_t)rr(8, 40);
    wr16(s + 8, (uint16_t)top); wr16(s + 0xA, (uint16_t)left); wr16(s + 0xC, (uint16_t)(top + h)); wr16(s + 0xE, (uint16_t)(left + w));
    int16_t otop = rr(0, 2) ? (int16_t)(top + rr(-6, 6)) : top, oleft = rr(0, 2) ? (int16_t)(left + rr(-6, 6)) : left;
    wr16(s + 0x10, (uint16_t)otop); wr16(s + 0x12, (uint16_t)oleft); wr16(s + 0x14, (uint16_t)(otop + h)); wr16(s + 0x16, (uint16_t)(oleft + w));
    wr16(s + 0x18, (uint16_t)w); wr16(s + 0x1A, (uint16_t)h);
    static const uint8_t flag[4] = { 0, 1, 0xFF, 0 };
    w8(s + 0x1C, rr(0, 3) ? 1 : 0); w8(s + 0x1D, flag[rr(0, 3)]); w8(s + 0x1E, flag[rr(0, 3)]); w8(s + 0x1F, rr(0, 5) == 0);
    uint32_t mcb = kMoveCbs[rr(0, 5)], fcb = kFrameCbs[rr(0, 5)];
    wr32(s + 0x20, rr(0, 5) == 0 ? g_a5 + 0x242 : mcb);
    wr32(s + 0x42, rr(0, 5) == 0 ? g_a5 + 0x24A : fcb);
    static const uint16_t iv[] = { 0xFFFF, 0, 1, 2, 5 };
    wr16(s + 0x28, iv[rr(0, 4)]); wr16(s + 0x50, iv[rr(0, 4)]);
    wr32(s + 0x24, now - (uint32_t)rr(0, 8)); wr32(s + 0x4C, now - (uint32_t)rr(0, 8));
    wr16(s + 0x46, (uint16_t)rr(2, 8)); wr16(s + 0x48, (uint16_t)rr(1, 8)); wr16(s + 0x4A, (uint16_t)rr(1, 8));
    wr32(s + 0x2A, fixed(0, 420)); wr32(s + 0x2E, fixed(0, 500));
    wr32(s + 0x3A, rr(0, 3) ? fixed(-4, 4) : 0); wr32(s + 0x3E, rr(0, 3) ? fixed(-4, 4) : 0);
    wr32(s + 0x52, rnd32() & 0xFFFF);
    /* append to the list */
    uint32_t last = rd32(list + 8);
    wr32(s + 0x56, last); wr32(s + 0x5A, 0);
    if (last) wr32(last + 0x5A, s);
    if (!rd32(list + 4)) wr32(list + 4, s);
    wr32(list + 8, s);
}

static void setup(int with_sprites)
{
    /* clear and fill the regions */
    memset(&g_mem[ENGINE], 0, 0x100);
    memset(&g_mem[LISTS], 0, 0x400);
    memset(&g_mem[SPRITES], 0, 0x2000);
    memset(&g_mem[HEAP], 0, 0x2000);
    for (uint32_t i = 0; i < 0x8000; i += 4) wr32(0x3B0000 + i, rnd32());
    for (int k = 0; k < NCLASS; k++) build_class(k);
    g_heap_next = HEAP; ctx.heap_next = HEAP;

    g5(-0x75A, 1, 1);
    g5(-0x758, ENGINE, 4);
    wr32(0x16A, rnd32() & 0xFFFFF);
    wr32(ENGINE + 0x14, BOUNDS);
    wr16(BOUNDS + 0x14, (uint16_t)rr(400, 500)); wr16(BOUNDS + 0x16, (uint16_t)rr(400, 460));
    wr32(ENGINE + 0x18, ROWS_SCREEN + 0x400); wr32(ENGINE + 0x1C, ROWS_WORK + 0x400); wr32(ENGINE + 0x20, ROWS_SAVED + 0x400);
    wr16(ENGINE + 0x24, (uint16_t)rr(0, 40)); wr16(ENGINE + 0x26, (uint16_t)rr(0, 40)); wr16(ENGINE + 0x28, (uint16_t)rr(0, 40));
    wr16(ENGINE + 0x2A, (uint16_t)rr(0, 3));
    wr32(ENGINE + 0x2C, rd32(0x16A) - (uint32_t)rr(0, 6));
    w8(ENGINE + 0x30, rr(0, 4) == 0 ? 1 : 0); w8(ENGINE + 0x31, rr(0, 4) == 0 ? 1 : 0);
    wr16(ENGINE + 0x3A, (uint16_t)rr(-8, 8)); wr16(ENGINE + 0x3C, (uint16_t)rr(-8, 8));
    /* other globals the callbacks use */
    g5(-0x654, (uint32_t)rr(-20, 20), 2);
    g5(-0x548, rnd32(), 4);
    nlists = 0; nsprites = 0;
    if (!with_sprites) return;
    int nl = rr(1, 4);
    uint32_t prev = 0;
    for (int i = 0; i < nl; i++) {
        uint32_t l = LISTS + (uint32_t)i * 0x20;
        wr32(l, rnd32()); wr32(l + 4, 0); wr32(l + 8, 0); w8(l + 0xC, rr(0, 4) ? 1 : 0); wr32(l + 0xE, prev); wr32(l + 0x12, 0);
        if (prev) wr32(prev + 0x12, l);
        if (!rd32(ENGINE + 4)) wr32(ENGINE + 4, l);
        wr32(ENGINE + 8, l);
        list_addr[i] = l; prev = l; nlists++;
        int ns = rr(0, 6);
        for (int j = 0; j < ns; j++) build_sprite(SPRITES + (nsprites++) * 0x100, l);
    }
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
        printf("  %s trial %ld: %d logged calls in 68k, %d in C\n", name, trial, g_ncalls, c_ncalls);
        if (getenv("SHOW")) for (int i = 0; i < g_ncalls || i < c_ncalls; i++) printf("     %2d  68k %06X   C %06X\n", i, i < g_ncalls ? g_calls[i].addr : 0, i < c_ncalls ? c_calls[i].addr : 0);
        return 1;
    }
    for (int i = 0; i < g_ncalls; i++)
        if (g_calls[i].addr != c_calls[i].addr || g_calls[i].n != c_calls[i].n || memcmp(g_calls[i].raw, c_calls[i].raw, (size_t)g_calls[i].n)) {
            printf("  %s trial %ld: call %d differs (68k %06X n=%d, C %06X n=%d)\n", name, trial, i, g_calls[i].addr, g_calls[i].n, c_calls[i].addr, c_calls[i].n);
            return 1;
        }
    return 0;
}

enum { T_LIST, T_ALLOC, T_UNLINK, T_ACTIVATE, T_OVERLAP, T_MOVE, T_DRAW, T_ALLOC_MANY };
static const struct { const char *name; uint32_t addr; int kind; } kCases[] = {
    { "seg3_0712 create list", 0x40712, T_LIST }, { "seg3_0782 allocate sprite", 0x40782, T_ALLOC },
    { "seg3_0884 unlink sprite", 0x40884, T_UNLINK }, { "seg3_0902 activate", 0x40902, T_ACTIVATE },
    { "seg3_055E overlap search", 0x4055E, T_OVERLAP }, { "seg3_00EA move pass", 0x400EA, T_MOVE },
    { "seg3_020C draw pass", 0x4020C, T_DRAW } };

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../../../apeiron_work";
    int trials = argc > 2 ? atoi(argv[2]) : 50000;
    if (image_load(dir)) return 2;
    cmem = (uint8_t *)calloc(MEM_SIZE, 1);
    ctx.mem = cmem; ctx.a5 = g_a5; ctx.ext = c_ext;
    g_regions[g_nreg++] = (Region){ A5_WORLD, g_world_len };
    g_regions[g_nreg++] = (Region){ ENGINE, 0x100 };
    g_regions[g_nreg++] = (Region){ LISTS, 0x400 };
    g_regions[g_nreg++] = (Region){ SPRITES, 0x2000 };
    g_regions[g_nreg++] = (Region){ HEAP, 0x2000 };
    g_regions[g_nreg++] = (Region){ 0x16A, 4 };
    g_regions[g_nreg++] = (Region){ BOUNDS, 0x100 };
    g_regions[g_nreg++] = (Region){ 0x3B0000, 0x8000 };
    g_regions[g_nreg++] = (Region){ HANDLES, 0x2000 };
    int total_bad = 0;
    for (size_t c = 0; c < sizeof kCases / sizeof kCases[0]; c++) {
        if (argc > 3 && !strstr(kCases[c].name, argv[3])) continue;
        harness_clear_stubs();
        harness_add_stub(g_a5 + 0x3C2u, 10);
        for (int k = 0; k < NCLASS; k++)
            for (int f = 0; f < 8; f++) {
                harness_add_stub(STUB_B + (uint32_t)k * 0x100 + (uint32_t)f * 8, -3);
                harness_add_stub(STUB_C + (uint32_t)k * 0x100 + (uint32_t)f * 8, -2);
            }
        int bad = 0;
        for (int t = 0; t < trials && bad < 5; t++) {
            int kind = kCases[c].kind;
            setup(kind != T_LIST && kind != T_ALLOC);
            if (kind == T_ALLOC) {                     /* needs a list to put the sprite in */
                uint32_t l = LISTS;
                wr32(l, rnd32()); wr32(l + 4, 0); wr32(l + 8, 0); w8(l + 0xC, 1); wr32(l + 0xE, 0); wr32(l + 0x12, 0);
                wr32(ENGINE + 4, l); wr32(ENGINE + 8, l); list_addr[0] = l; nlists = 1;
                if (rr(0, 1)) build_sprite(SPRITES, l), nsprites = 1;
            }
            snapshot_to_c();
            c_ncalls = 0;
            uint32_t args[3], d0 = 0, cd0 = 0;
            int wide[3] = { 1, 1, 1 }, nargs = 0;
            uint32_t s = nsprites ? SPRITES + (uint32_t)rr(0, (int)nsprites - 1) * 0x100 : SPRITES;
            uint32_t l = nlists ? list_addr[rr(0, (int)nlists - 1)] : LISTS;
            switch (kind) {
            case T_LIST: args[0] = rnd32(); nargs = 1; break;
            case T_ALLOC: args[0] = HANDLES + (uint32_t)rr(0, NCLASS - 1) * 4; args[1] = rnd32(); args[2] = list_addr[0]; nargs = 3; break;
            case T_UNLINK: args[0] = l; args[1] = rd32(l + 4) ? rd32(l + 4) : s; nargs = 2; if (rr(0, 1) && rd32(l + 8)) args[1] = rd32(l + 8); break;
            case T_ACTIVATE: args[0] = s; nargs = 1; break;
            case T_OVERLAP: args[0] = s; args[1] = l; nargs = 2; break;
            default: break;
            }
            if (kind == T_UNLINK && !rd32(l + 4)) { t--; continue; }
            { extern int g_trace; g_trace = getenv("TT") && t == atoi(getenv("TT")); }
            d0 = m68k_call(kCases[c].addr, args, wide, nargs, 400000);
            switch (kind) {
            case T_LIST: cd0 = engine_list_create(&ctx, args[0]); break;
            case T_ALLOC: cd0 = engine_sprite_alloc(&ctx, args[0], args[1], args[2]); break;
            case T_UNLINK: engine_unlink(&ctx, args[0], args[1]); cd0 = d0; break;
            case T_ACTIVATE: engine_activate(&ctx, args[0]); cd0 = d0; break;
            case T_OVERLAP: cd0 = engine_overlap(&ctx, args[0], args[1]); break;
            case T_MOVE: engine_move_pass(&ctx); cd0 = d0; break;
            case T_DRAW: if (getenv("DBG2")) { for (uint32_t q = 0; q < nsprites; q++) { uint32_t sp = SPRITES + q * 0x100; uint32_t cl = rd32(sp + 4), dt = rd32(cl); int fr = (int16_t)(rd16(sp + 0x4A) - 1); uint32_t tb = dt + rd32(dt + 0x12); printf("  sprite %06X class %06X data %06X frame-1 %d table %06X entry %08X\n", sp, cl, dt, fr, tb, rd32(tb + (uint32_t)(fr * 4))); } }
if (getenv("DBG")) printf("   interval %d ticks %u last %u drawn %d moved %d\n", (int16_t)rd16(ENGINE + 0x2A), rd32(0x16A), rd32(ENGINE + 0x2C), g_mem[ENGINE + 0x31], g_mem[ENGINE + 0x30]); engine_draw_pass(&ctx); cd0 = d0; break;
            }
            if (compare(kCases[c].name, t)) bad++;
            else if ((kind == T_LIST || kind == T_ALLOC || kind == T_OVERLAP) && d0 != cd0) { printf("  %s trial %d: returns %08X vs %08X\n", kCases[c].name, t, d0, cd0); bad++; }
        }
        printf("%-30s %d trials, %d mismatches\n", kCases[c].name, trials, bad);
        total_bad += bad;
    }
    return total_bad ? 1 : 0;
}
