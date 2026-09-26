// Canvas geometry and text anchoring, ported from the reference
// engine/terminal.py Canvas.
#ifndef GLYPHFX_CANVAS_H
#define GLYPHFX_CANVAS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "engine/character.h"
#include "utils/geometry.h"

typedef enum {
    ANCHOR_N,
    ANCHOR_NE,
    ANCHOR_E,
    ANCHOR_SE,
    ANCHOR_S,
    ANCHOR_SW,
    ANCHOR_W,
    ANCHOR_NW,
    ANCHOR_C,
} Anchor;

// Parses the two-letter anchor name; returns false on unknown strings.
bool anchor_parse(const char *s, Anchor *out);

typedef struct {
    int64_t top;
    int64_t right;
    int64_t bottom;
    int64_t left;
    int64_t center_row;
    int64_t center_column;
    Coord center;
    int64_t width;
    int64_t height;
    int64_t text_left;
    int64_t text_right;
    int64_t text_top;
    int64_t text_bottom;
    int64_t text_width;
    int64_t text_height;
    int64_t text_center_row;
    int64_t text_center_column;
    Coord text_center;
} Canvas;

Canvas canvas_new(int64_t top, int64_t right);
bool canvas_coord_is_in_canvas(const Canvas *c, Coord coord);
bool canvas_coord_is_in_text(const Canvas *c, Coord coord);

// Shifts characters per the anchor, drops out-of-canvas ones, then computes
// text extents. Returns 0 and sets *out_kept/*out_n on success; -1 on
// empty-input like the reference. On success *out_kept is malloc'd.
int canvas_anchor_text(Canvas *canvas, Arena *arena, CharId *chars, size_t n, Anchor anchor,
                       CharId **out_kept, size_t *out_n);

#endif
