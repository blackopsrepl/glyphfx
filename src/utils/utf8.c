#include "utils/utf8.h"

#include <stdlib.h>

size_t utf8_count_codepoints(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

static int is_cont(uint8_t b) {
    return (b & 0xC0) == 0x80;
}

int utf8_decode_strict(const char *bytes, size_t len, uint32_t **out_cps, size_t *out_count,
                       size_t *bad_offset) {
    uint32_t *cps = malloc((len ? len : 1) * sizeof(uint32_t));
    if (!cps) {
        return -1;
    }
    size_t n = 0;
    size_t i = 0;
    while (i < len) {
        uint8_t b0 = (uint8_t)bytes[i];
        uint32_t cp = 0;
        size_t need = 0;
        if (b0 < 0x80) {
            cp = b0;
            need = 1;
        } else if ((b0 & 0xE0) == 0xC0) {
            cp = b0 & 0x1F;
            need = 2;
        } else if ((b0 & 0xF0) == 0xE0) {
            cp = b0 & 0x0F;
            need = 3;
        } else if ((b0 & 0xF8) == 0xF0) {
            cp = b0 & 0x07;
            need = 4;
        } else {
            free(cps);
            if (bad_offset) {
                *bad_offset = i;
            }
            return -1;
        }
        if (i + need > len) {
            free(cps);
            if (bad_offset) {
                *bad_offset = i;
            }
            return -1;
        }
        for (size_t k = 1; k < need; k++) {
            uint8_t b = (uint8_t)bytes[i + k];
            if (!is_cont(b)) {
                free(cps);
                if (bad_offset) {
                    *bad_offset = i;
                }
                return -1;
            }
            cp = (cp << 6) | (b & 0x3F);
        }
        // Reject overlong encodings, surrogates, and out-of-range values.
        static const uint32_t mins[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (cp < mins[need] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            free(cps);
            if (bad_offset) {
                *bad_offset = i;
            }
            return -1;
        }
        cps[n++] = cp;
        i += need;
    }
    *out_cps = cps;
    *out_count = n;
    return 0;
}
