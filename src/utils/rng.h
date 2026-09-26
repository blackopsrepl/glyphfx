// Engine RNG: xoshiro256++ with Python-random-shaped helpers.
//
// The helper algorithms are the parity contract; they must match the reference
// byte for byte. The RNG lives on the engine context; there are no globals.
#ifndef GLYPHFX_RNG_H
#define GLYPHFX_RNG_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t s[4];
} Rng;

Rng rng_seeded(uint64_t seed);
Rng rng_from_entropy(void);

uint64_t rng_next_u64(Rng *rng);
double rng_random(Rng *rng);
int64_t rng_randint(Rng *rng, int64_t a, int64_t b);
int64_t rng_randrange(Rng *rng, int64_t a, int64_t b);
// choice over an array of nbytes-wide elements; returns the chosen index.
size_t rng_choice_index(Rng *rng, size_t len);
double rng_uniform(Rng *rng, double a, double b);
void rng_shuffle(Rng *rng, void *base, size_t count, size_t elem_size);

#endif
