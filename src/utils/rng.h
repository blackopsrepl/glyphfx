// Engine RNG: xoshiro256++ with Python-random-shaped helpers.
//
// The helper algorithms are the parity contract; they must match the reference
// byte for byte. The RNG lives on the engine context; there are no globals.
#ifndef GLYPHFX_RNG_H
#define GLYPHFX_RNG_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    uint64_t s[4];
} Rng;

Rng rng_seeded(uint64_t seed);
Rng rng_from_entropy(void);

// The two hottest primitives are inlined: effects like matrix draw one value
// per character per frame, so the call overhead dominated the draw itself. The
// arithmetic is unchanged.
static inline uint64_t rng_rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static inline uint64_t rng_next_u64(Rng *rng) {
    uint64_t *s = rng->s;
    uint64_t result = rng_rotl64(s[0] + s[3], 23) + s[0];
    uint64_t t = s[1] << 17;
    s[2] ^= s[0];
    s[3] ^= s[1];
    s[1] ^= s[2];
    s[0] ^= s[3];
    s[2] ^= t;
    s[3] = rng_rotl64(s[3], 45);
    return result;
}

static inline double rng_random(Rng *rng) {
    uint64_t v = rng_next_u64(rng) >> 11;
    return (double)v * (1.0 / 9007199254740992.0);  // 1 / 2^53
}

// The rejection sampler is the parity contract, so it stays scalar; it is
// inlined only to remove the call that dominated a one-draw-per-character
// effect (matrix, synthgrid).
static inline uint64_t rng_randbelow(Rng *rng, uint64_t n) {
    if (n == 0) {
        fputs("glyphfx: randbelow(0)\n", stderr);
        abort();
    }
    // n == 1 makes n - 1 zero, where __builtin_clzll is undefined; the
    // reference takes one bit there (getrandbits(1) rejecting a 1), so the
    // width is 1. The reference's leading_zeros64(0) is 64, giving the same.
    uint64_t nm1 = n - 1;
    int bits = nm1 == 0 ? 1 : 64 - __builtin_clzll(nm1);
    if (bits < 1) {
        bits = 1;
    }
    for (;;) {
        uint64_t r = rng_next_u64(rng) >> (64 - bits);
        if (r < n) {
            return r;
        }
    }
}

static inline size_t rng_choice_index(Rng *rng, size_t len) {
    if (len == 0) {
        fputs("glyphfx: choice on empty sequence\n", stderr);
        abort();
    }
    return (size_t)rng_randbelow(rng, (uint64_t)len);
}

static inline int64_t rng_randint(Rng *rng, int64_t a, int64_t b) {
    if (a > b) {
        fputs("glyphfx: randint range empty\n", stderr);
        abort();
    }
    return a + (int64_t)rng_randbelow(rng, (uint64_t)(b - a) + 1);
}

static inline int64_t rng_randrange(Rng *rng, int64_t a, int64_t b) {
    if (a >= b) {
        fputs("glyphfx: randrange range empty\n", stderr);
        abort();
    }
    return a + (int64_t)rng_randbelow(rng, (uint64_t)(b - a));
}
double rng_uniform(Rng *rng, double a, double b);
void rng_shuffle(Rng *rng, void *base, size_t count, size_t elem_size);

#endif
