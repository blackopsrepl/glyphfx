// Canvas geometry and text anchoring, ported from the reference
// engine/terminal.py Canvas.
#ifndef GLYPHFX_CANVAS_H
#define GLYPHFX_CANVAS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "engine/character.h"
#include "utils/geometry.h"
#include "utils/rng.h"

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

int64_t canvas_random_column(const Canvas *c, Rng *rng, bool within_text_boundary);
int64_t canvas_random_row(const Canvas *c, Rng *rng, bool within_text_boundary);
// outside_scope picks among four coords one cell past an edge; the RNG call
// order (above, below, left, right, then choice) is part of the contract.
Coord canvas_random_coord(const Canvas *c, Rng *rng, bool outside_scope, bool within_text_boundary);

// Shifts characters per the anchor, drops out-of-canvas ones, then computes
// text extents. Returns 0 and sets *out_kept/*out_n on success; -1 on
// empty-input like the reference. On success *out_kept is malloc'd.
int canvas_anchor_text(Canvas *canvas, Arena *arena, CharId *chars, size_t n, Anchor anchor,
                       CharId **out_kept, size_t *out_n);

#endif
