// Shared string hash. FNV-1a over the bytes, used by the string-keyed map and
// the event caller keys so a miss is an integer compare instead of strcmp.
#ifndef GLYPHFX_STRHASH_H
#define GLYPHFX_STRHASH_H

#include <stdint.h>

static inline uint64_t str_hash64(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

#endif
