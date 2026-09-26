#include "utils/hexterm.h"

#include <string.h>

#include "utils/hexterm_table.h"

static void parse_rgb6(const char *hex_color, uint8_t rgb[3]) {
    // Reads the first six hex digits, ignoring anything else (the reference
    // accepts seven-digit strings and parses only the first six).
    const char *s = hex_color;
    while (*s == '#') {
        s++;
    }
    int vals[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        int v = 0;
        for (int d = 0; d < 2; d++) {
            char c = s[i * 2 + d];
            int digit;
            if (c >= '0' && c <= '9') {
                digit = c - '0';
            } else if (c >= 'a' && c <= 'f') {
                digit = c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                digit = c - 'A' + 10;
            } else {
                digit = 0;
            }
            v = v * 16 + digit;
        }
        vals[i] = v;
    }
    rgb[0] = (uint8_t)vals[0];
    rgb[1] = (uint8_t)vals[1];
    rgb[2] = (uint8_t)vals[2];
}

uint8_t hex_to_xterm(const char *hex_color) {
    uint8_t rgb[3];
    parse_rgb6(hex_color, rgb);
    int min_diff = -1;
    uint8_t closest = 0;
    for (int code = 0; code < 256; code++) {
        uint8_t other[3];
        parse_rgb6(XTERM_TO_HEX[code], other);
        int diff = (int)(rgb[0] > other[0] ? rgb[0] - other[0] : other[0] - rgb[0]) +
                   (int)(rgb[1] > other[1] ? rgb[1] - other[1] : other[1] - rgb[1]) +
                   (int)(rgb[2] > other[2] ? rgb[2] - other[2] : other[2] - rgb[2]);
        if (min_diff < 0 || diff < min_diff) {
            min_diff = diff;
            closest = (uint8_t)code;
        }
    }
    return closest;
}

const char *xterm_to_hex(uint8_t xterm_color) {
    return XTERM_TO_HEX[xterm_color];
}

bool is_valid_hex_color(const char *color) {
    const char *start = color;
    while (*start == '#') {
        start++;
    }
    size_t stripped_len = strlen(start);
    if (stripped_len != 6 && stripped_len != 7) {
        return false;
    }
    // Parse the value with both leading and trailing '#' trimmed.
    const char *p = color;
    const char *end = color + strlen(color);
    while (p < end && *p == '#') {
        p++;
    }
    while (end > p && end[-1] == '#') {
        end--;
    }
    if (p == end) {
        return false;
    }
    for (const char *q = p; q < end; q++) {
        char c = *q;
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}
