#include "utils/ansi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Reads up to six hex digits starting at s; missing/odd digits count as 0.
// Returns the channel triple. Reads past the string only if it is shorter than
// six digits, in which case the NUL and following bytes cannot be hex digits.
static void hex6_channels(const char *s, uint8_t rgb[3]) {
    for (int i = 0; i < 3; i++) {
        int hi = hex_digit(s[i * 2]);
        int lo = hex_digit(s[i * 2 + 1]);
        if (hi < 0) {
            hi = 0;
        }
        if (lo < 0) {
            lo = 0;
        }
        rgb[i] = (uint8_t)(hi * 16 + lo);
    }
}

ColorCode colorcode_rgb(const char *hex) {
    ColorCode code;
    code.kind = COLORCODE_RGB;
    code.xterm = 0;
    size_t len = strlen(hex);
    if (len >= sizeof(code.hex)) {
        len = sizeof(code.hex) - 1;
    }
    memcpy(code.hex, hex, len);
    code.hex[len] = '\0';
    return code;
}

ColorCode colorcode_xterm(uint8_t code) {
    ColorCode cc;
    cc.kind = COLORCODE_XTERM;
    cc.hex[0] = '\0';
    cc.xterm = code;
    return cc;
}

// Digits without going through core::fmt / snprintf: every restyled character
// reassembles its SGR string, so this is on the effect-build hot path.
// Writes the decimal digits of a non-negative int, returns the count.
static size_t put_uint(char *p, int value) {
    if (value == 0) {
        p[0] = '0';
        return 1;
    }
    char digits[12];
    int n = 0;
    while (value > 0) {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    }
    for (int i = 0; i < n; i++) {
        p[i] = digits[n - 1 - i];
    }
    return (size_t)n;
}

// Builds the whole SGR sequence in a stack buffer and appends it once; every
// restyled character reassembles its SGR string, so this is a hot path.
static void sgr_color(const ColorCode *code, int location, StrBuf *out) {
    char buf[48];
    size_t n = 0;
    buf[n++] = '\x1b';
    buf[n++] = '[';
    n += put_uint(buf + n, location);
    if (code->kind == COLORCODE_RGB) {
        uint8_t rgb[3];
        hex6_channels(code->hex, rgb);
        buf[n++] = ';';
        buf[n++] = '2';
        buf[n++] = ';';
        n += put_uint(buf + n, rgb[0]);
        buf[n++] = ';';
        n += put_uint(buf + n, rgb[1]);
        buf[n++] = ';';
        n += put_uint(buf + n, rgb[2]);
    } else {
        buf[n++] = ';';
        buf[n++] = '5';
        buf[n++] = ';';
        n += put_uint(buf + n, code->xterm);
    }
    buf[n++] = 'm';
    sb_append(out, buf, n);
}

void ansi_fg(const ColorCode *code, StrBuf *out) {
    sgr_color(code, 38, out);
}

void ansi_bg(const ColorCode *code, StrBuf *out) {
    sgr_color(code, 48, out);
}

void ansi_move_cursor_up(StrBuf *out, int64_t y) {
    sb_puts(out, "\x1b[");
    if (y == 0) {
        sb_push(out, '0');
    } else {
        char digits[24];
        int n = 0;
        while (y > 0) {
            digits[n++] = (char)('0' + (int)(y % 10));
            y /= 10;
        }
        while (n > 0) {
            sb_push(out, digits[--n]);
        }
    }
    sb_push(out, 'A');
}

static const char *strip_prefix(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    if (strncmp(s, prefix, n) == 0) {
        return s + n;
    }
    return NULL;
}

int ansi_parse_color_sequence(const char *sequence, ColorCode *out) {
    const char *s = sequence;
    const char *after = strip_prefix(s, "\x1b[");
    if (after) {
        s = after;
    }
    // trim_matches('m'): trim leading and trailing 'm'.
    while (*s == 'm') {
        s++;
    }
    size_t s_len = strlen(s);
    while (s_len > 0 && s[s_len - 1] == 'm') {
        s_len--;
    }
    char work[64];
    if (s_len >= sizeof(work)) {
        s_len = sizeof(work) - 1;
    }
    memcpy(work, s, s_len);
    work[s_len] = '\0';
    s = work;

    const char *rest = strip_prefix(s, "38;2");
    if (!rest) {
        rest = strip_prefix(s, "48;2");
    }
    if (rest) {
        if (*rest == ';') {
            rest++;
        }
        char hex[64];
        size_t hlen = 0;
        const char *p = rest;
        for (;;) {
            const char *semi = strchr(p, ';');
            size_t flen = semi ? (size_t)(semi - p) : strlen(p);
            long long v = 0;
            if (flen > 0) {
                char field[32];
                if (flen >= sizeof(field)) {
                    return -1;
                }
                memcpy(field, p, flen);
                field[flen] = '\0';
                char *endp = NULL;
                v = strtoll(field, &endp, 10);
                if (!endp || *endp != '\0') {
                    return -1;
                }
            }
            char piece[32];
            int n = snprintf(piece, sizeof(piece), "%02llX", (unsigned long long)v);
            if (n < 0 || hlen + (size_t)n + 1 > sizeof(hex)) {
                return -1;
            }
            memcpy(hex + hlen, piece, (size_t)n);
            hlen += (size_t)n;
            if (!semi) {
                break;
            }
            p = semi + 1;
        }
        hex[hlen] = '\0';
        *out = colorcode_rgb(hex);
        return 0;
    }

    rest = strip_prefix(s, "38;5");
    if (!rest) {
        rest = strip_prefix(s, "48;5");
    }
    if (rest) {
        if (*rest == ';') {
            rest++;
        }
        char *endp = NULL;
        long long v = strtoll(rest, &endp, 10);
        if (!endp || *endp != '\0' || rest[0] == '\0') {
            return -1;
        }
        *out = colorcode_xterm((uint8_t)v);
        return 0;
    }

    return -1;
}
