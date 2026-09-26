// xterm-256 <-> RGB conversion, ported from the reference utils/hexterm.py.
#ifndef GLYPHFX_HEXTERM_H
#define GLYPHFX_HEXTERM_H

#include <stdbool.h>
#include <stdint.h>

// Closest xterm-256 code by mean absolute channel difference: linear scan over
// codes 0..=255, strict '<' so the first minimum wins.
uint8_t hex_to_xterm(const char *hex_color);

// xterm code -> "rrggbb" without a leading '#'.
const char *xterm_to_hex(uint8_t xterm_color);

// 6 (or, faithfully to the reference, 7) hex digits with optional leading '#'.
bool is_valid_hex_color(const char *color);

#endif
