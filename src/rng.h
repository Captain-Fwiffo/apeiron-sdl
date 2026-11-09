/* Apeiron's random number generator (CODE 9, 0xA014E): a Lehmer generator, multiplier 16807 (0x41A7), modulus 2^31-1,
 * kept in a 32-bit seed. Re-implemented from the decompilation and checked against the original 68k code (verify/musashi). */
#ifndef APEIRON_RNG_H
#define APEIRON_RNG_H

#include <stdint.h>

/* n == 0: reseeds from the Mac clock plus the tick count ("seed_value" is Time + Ticks), returns 0.
 * n  > 0: advances the seed and returns a number in 0..n-1 (n is an unsigned 16-bit value). */
uint16_t rng_random(uint32_t *seed, uint16_t n, uint32_t seed_value);

#endif
