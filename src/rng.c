#include "rng.h"

uint16_t rng_random(uint32_t *seed, uint16_t n, uint32_t seed_value)
{
    if (n == 0) {
        *seed = seed_value;
        return 0;
    }
    uint32_t lo = (*seed & 0xFFFF) * 0x41A7u;
    uint32_t t = (lo >> 16) + (*seed >> 16) * 0x41A7u;
    uint32_t s = ((t & 0x7FFFFFFFu) >> 15) + (t & 0x7FFF) * 0x10000u + (lo & 0xFFFF) + 0x80000001u;
    if ((int32_t)s < 0) s += 0x7FFFFFFFu;
    *seed = s;
    uint32_t r = s;
    if ((uint16_t)r == 0x8000) r &= 0xFFFF0000u;          /* the original clears the low word in this one case */
    return (uint16_t)(((uint32_t)n * (r & 0xFFFF)) >> 16);
}
