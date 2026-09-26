#include "utils/graphics.h"

#include <string.h>

#include "utils/hexterm.h"

static uint8_t hex_byte(const char *s) {
    int hi = 0;
    int lo = 0;
    char a = s[0];
    char b = s[1];
    if (a >= '0' && a <= '9') {
        hi = a - '0';
    } else if (a >= 'a' && a <= 'f') {
        hi = a - 'a' + 10;
    } else if (a >= 'A' && a <= 'F') {
        hi = a - 'A' + 10;
    }
    if (b >= '0' && b <= '9') {
        lo = b - '0';
    } else if (b >= 'a' && b <= 'f') {
        lo = b - 'a' + 10;
    } else if (b >= 'A' && b <= 'F') {
        lo = b - 'A' + 10;
    }
    return (uint8_t)(hi * 16 + lo);
}

static void parse_rgb6(const char *s, uint8_t rgb[3]) {
    rgb[0] = hex_byte(s);
    rgb[1] = hex_byte(s + 2);
    rgb[2] = hex_byte(s + 4);
}

bool color_eq(const Color *a, const Color *b) {
    if (a->is_xterm != b->is_xterm) {
        return false;
    }
    if (a->is_xterm) {
        return a->xterm == b->xterm;
    }
    return a->hex_len == b->hex_len && memcmp(a->hex, b->hex, a->hex_len) == 0;
}

Color color_from_xterm(uint8_t code) {
    Color c;
    c.is_xterm = true;
    c.xterm = code;
    const char *h = xterm_to_hex(code);
    size_t len = strlen(h);
    memcpy(c.hex, h, len);
    c.hex[len] = '\0';
    c.hex_len = (uint8_t)len;
    parse_rgb6(h, c.rgb);
    return c;
}

int color_from_hex(const char *hex, Color *out) {
    // trim_matches('#'): leading and trailing '#' removed.
    const char *p = hex;
    while (*p == '#') {
        p++;
    }
    size_t len = strlen(p);
    while (len > 0 && p[len - 1] == '#') {
        len--;
    }
    char stripped[8];
    if (len >= sizeof(stripped)) {
        return -1;
    }
    memcpy(stripped, p, len);
    stripped[len] = '\0';
    if (!is_valid_hex_color(stripped)) {
        return -1;
    }
    Color c;
    c.is_xterm = false;
    c.xterm = 0;
    memcpy(c.hex, stripped, len);
    c.hex[len] = '\0';
    c.hex_len = (uint8_t)len;
    parse_rgb6(stripped, c.rgb);
    *out = c;
    return 0;
}

Color color_from_rgb(uint8_t r, uint8_t g, uint8_t b) {
    static const char digits[] = "0123456789abcdef";
    Color c;
    c.is_xterm = false;
    c.xterm = 0;
    c.hex[0] = digits[(r >> 4) & 0xF];
    c.hex[1] = digits[r & 0xF];
    c.hex[2] = digits[(g >> 4) & 0xF];
    c.hex[3] = digits[g & 0xF];
    c.hex[4] = digits[(b >> 4) & 0xF];
    c.hex[5] = digits[b & 0xF];
    c.hex[6] = '\0';
    c.hex_len = 6;
    c.rgb[0] = r;
    c.rgb[1] = g;
    c.rgb[2] = b;
    return c;
}

void color_rgb_ints(const Color *c, uint8_t rgb[3]) {
    rgb[0] = c->rgb[0];
    rgb[1] = c->rgb[1];
    rgb[2] = c->rgb[2];
}

const char *color_hex_text(const Color *c) {
    return c->hex;
}
