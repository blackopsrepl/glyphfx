#include "utils/rng.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Rng rng_seeded(uint64_t seed) {
    uint64_t sm = seed;
    Rng rng;
    for (int i = 0; i < 4; i++) {
        sm += 0x9E3779B97F4A7C15ULL;
        uint64_t z = sm;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        rng.s[i] = z ^ (z >> 31);
    }
    return rng;
}

Rng rng_from_entropy(void) {
    uint8_t buf[8];
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f || fread(buf, 1, sizeof(buf), f) != sizeof(buf)) {
        if (f) {
            fclose(f);
        }
        fputs("glyphfx: failed to read /dev/urandom\n", stderr);
        abort();
    }
    fclose(f);
    uint64_t seed = 0;
    for (int i = 0; i < 8; i++) {
        seed |= (uint64_t)buf[i] << (8 * i);
    }
    return rng_seeded(seed);
}

static int leading_zeros64(uint64_t x) {
    if (x == 0) {
        return 64;
    }
    int n = 0;
    while ((x & 0x8000000000000000ULL) == 0) {
        x <<= 1;
        n++;
    }
    return n;
}

static uint64_t randbelow(Rng *rng, uint64_t n) {
    if (n == 0) {
        fputs("glyphfx: randbelow(0)\n", stderr);
        abort();
    }
    int bits = 64 - leading_zeros64(n - 1);
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

int64_t rng_randint(Rng *rng, int64_t a, int64_t b) {
    if (a > b) {
        fputs("glyphfx: randint range empty\n", stderr);
        abort();
    }
    uint64_t span = (uint64_t)(b - a) + 1;
    return a + (int64_t)randbelow(rng, span);
}

int64_t rng_randrange(Rng *rng, int64_t a, int64_t b) {
    if (a >= b) {
        fputs("glyphfx: randrange range empty\n", stderr);
        abort();
    }
    return a + (int64_t)randbelow(rng, (uint64_t)(b - a));
}

size_t rng_choice_index(Rng *rng, size_t len) {
    if (len == 0) {
        fputs("glyphfx: choice on empty sequence\n", stderr);
        abort();
    }
    return (size_t)randbelow(rng, (uint64_t)len);
}

double rng_uniform(Rng *rng, double a, double b) {
    return a + (b - a) * rng_random(rng);
}

void rng_shuffle(Rng *rng, void *base, size_t count, size_t elem_size) {
    if (count < 2) {
        return;
    }
    char *bytes = base;
    char tmp[64];
    char *scratch = elem_size <= sizeof(tmp) ? tmp : malloc(elem_size);
    for (size_t i = count - 1; i >= 1; i--) {
        size_t j = (size_t)randbelow(rng, (uint64_t)(i + 1));
        if (i == j) {
            continue;
        }
        memcpy(scratch, bytes + i * elem_size, elem_size);
        memcpy(bytes + i * elem_size, bytes + j * elem_size, elem_size);
        memcpy(bytes + j * elem_size, scratch, elem_size);
    }
    if (scratch != tmp) {
        free(scratch);
    }
}
