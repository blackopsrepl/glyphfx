// Color and gradient support, ported from the reference utils/graphics.py.
#ifndef GLYPHFX_GRAPHICS_H
#define GLYPHFX_GRAPHICS_H

#include <stdbool.h>
#include <stdint.h>

// A color preserves its original constructor argument because the reference's
// Color.__eq__/__hash__ compare color_arg: Color(255) != Color("ffffff") even
// when they resolve to the same RGB. Keying depends on this.
typedef struct {
    bool is_xterm;
    uint8_t xterm;
    char hex[8]; // stripped of '#', case preserved; at most 7 digits
    uint8_t hex_len;
    uint8_t rgb[3];
} Color;

typedef struct {
    bool has_fg;
    Color fg;
    bool has_bg;
    Color bg;
} ColorPair;

bool color_eq(const Color *a, const Color *b);

Color color_from_xterm(uint8_t code);
// Returns 0 on success, -1 on invalid hex (mirrors the reference ValueError).
int color_from_hex(const char *hex, Color *out);
Color color_from_rgb(uint8_t r, uint8_t g, uint8_t b);
void color_rgb_ints(const Color *c, uint8_t rgb[3]);
const char *color_hex_text(const Color *c);

#endif
