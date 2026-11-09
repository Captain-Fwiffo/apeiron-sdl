/* Differential test: seg9_014E (the RNG) in the original 68k code against src/rng.c. */
#include <stdio.h>
#include <stdlib.h>

#include "../../src/rng.h"
#include "harness.h"

#define RNG_ADDR 0xA014Eu
#define SEED_ADDR 0xA01C2u   /* the seed lives inside the CODE 9 segment, right after the routine */

static uint32_t xs = 0x12345678u;
static uint32_t next_u32(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "../../../apeiron_work";
    if (image_load(dir)) return 2;
    long checks = 0, bad = 0;
    for (int trial = 0; trial < 20000; trial++) {
        uint32_t seed = next_u32();
        if (trial < 8) seed = (uint32_t[]){ 0, 1, 0x7FFFFFFE, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, 0x00008000u, 0x41A7u }[trial];
        uint16_t n = (uint16_t)(trial % 3 == 0 ? next_u32() : (next_u32() % 300) + 1);
        if (n == 0) n = 1;
        uint32_t mine = seed;
        uint16_t want = rng_random(&mine, n, 0);
        wr32(SEED_ADDR, seed);
        uint32_t args[1] = { n };
        int wide[1] = { 0 };
        uint32_t d0 = m68k_call(RNG_ADDR, args, wide, 1, 10000);
        uint32_t seed_after = rd32(SEED_ADDR);
        checks++;
        if ((uint16_t)d0 != want || seed_after != mine) {
            if (bad++ < 10) printf("MISMATCH seed=%08X n=%u: 68k D0=%04X seed=%08X, C %04X seed=%08X\n", seed, n, (uint16_t)d0, seed_after, want, mine);
        }
    }
    /* n == 0 reseeds from Time (0x20C) + Ticks (0x16A) */
    wr32(0x20C, 0x11111111u); wr32(0x16A, 0x00002222u);
    uint32_t args0[1] = { 0 }; int w0[1] = { 0 };
    m68k_call(RNG_ADDR, args0, w0, 1, 10000);
    uint32_t seed0 = 0;
    rng_random(&seed0, 0, 0x11111111u + 0x2222u);
    checks++;
    if (rd32(SEED_ADDR) != seed0) { bad++; printf("MISMATCH reseed: 68k %08X, C %08X\n", rd32(SEED_ADDR), seed0); }
    printf("seg9_014E (RNG): %ld checks, %ld mismatches\n", checks, bad);
    return bad ? 1 : 0;
}
