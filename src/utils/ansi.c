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

static void push_decimal(StrBuf *out, int value) {
    char buf[8];
    (void)snprintf(buf, sizeof(buf), "%d", value);
    sb_puts(out, buf);
}

static void sgr_color(const ColorCode *code, int location, StrBuf *out) {
    sb_puts(out, "\x1b[");
    push_decimal(out, location);
    if (code->kind == COLORCODE_RGB) {
        uint8_t rgb[3];
        hex6_channels(code->hex, rgb);
        sb_puts(out, ";2;");
        push_decimal(out, rgb[0]);
        sb_push(out, ';');
        push_decimal(out, rgb[1]);
        sb_push(out, ';');
        push_decimal(out, rgb[2]);
    } else {
        sb_puts(out, ";5;");
        push_decimal(out, code->xterm);
    }
    sb_push(out, 'm');
}

void ansi_fg(const ColorCode *code, StrBuf *out) {
    sgr_color(code, 38, out);
}

void ansi_bg(const ColorCode *code, StrBuf *out) {
    sgr_color(code, 48, out);
}

void ansi_move_cursor_up(StrBuf *out, int64_t y) {
    char buf[32];
    (void)snprintf(buf, sizeof(buf), "\x1b[%lldA", (long long)y);
    sb_puts(out, buf);
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
