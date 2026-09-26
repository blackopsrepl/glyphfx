// Color and gradient support, ported from the reference utils/graphics.py.
#ifndef GLYPHFX_GRAPHICS_H
#define GLYPHFX_GRAPHICS_H

#include <stdbool.h>
#include <stdint.h>

#include "utils/geometry.h"
#include "utils/rng.h"

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

// --- gradient -------------------------------------------------------------

typedef enum {
    GRADIENT_VERTICAL,
    GRADIENT_HORIZONTAL,
    GRADIENT_RADIAL,
    GRADIENT_DIAGONAL,
} GradientDirection;

// The generated spectrum is NOT float lerp: channel deltas use Python integer
// floor division and the exact end stop is appended per pair.
typedef struct {
    Color *spectrum;
    size_t len;
} Gradient;

// Returns 0 on success, -1 on error. `steps_was_int` mirrors the reference
// quirk that only scalar (int) steps are validated before generation.
int gradient_new(const Color *stops, size_t n_stops, const int64_t *steps, size_t n_steps, bool steps_was_int,
                 bool do_loop, Gradient *out);
int gradient_with_steps(const Color *stops, size_t n_stops, int64_t steps, bool do_loop, Gradient *out);
void gradient_free(Gradient *g);
// Returns the color for a fraction, or NULL when the fraction is outside [0,1].
const Color *gradient_get_color_at_fraction(const Gradient *g, double fraction);

// Insertion-ordered Coord -> Color mapping over a rectangle.
typedef struct {
    int64_t min_row;
    int64_t min_column;
    size_t width;
    size_t height;
    bool column_major;
    Color *colors;
    Coord *order;
    size_t len;
} CoordColorMap;

void coordcolormap_free(CoordColorMap *m);
const Color *coordcolormap_get(const CoordColorMap *m, Coord coord);
int gradient_build_coordinate_color_mapping(const Gradient *g, int64_t min_row, int64_t max_row, int64_t min_column,
                                            int64_t max_column, GradientDirection direction, CoordColorMap *out);

Color random_color(Rng *rng);
// Returns 0 on success, -1 when the extrapolated hex is invalid; a leading
// minus channel is an error (mirrors the reference panic/error conditions).
int shift_color_towards(const Color *color, const Color *target, double factor, Color *out);
Color color_adjust_brightness(const Color *color, double brightness);

#endif
