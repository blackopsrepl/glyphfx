// matrix, ported from the reference effects/matrix.rs.
#ifndef GLYPHFX_EFFECT_MATRIX_H
#define GLYPHFX_EFFECT_MATRIX_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    Color highlight_color;
    ColorList rain_color_gradient;
    StringList rain_symbols;
    IntRange rain_fall_delay_range;
    IntRange rain_column_delay_range;
    int64_t rain_time;
    double symbol_swap_chance;
    double color_swap_chance;
    int64_t resolve_delay;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    int64_t final_gradient_frames;
    GradientDirection final_gradient_direction;
} MatrixConfig;

void matrix_config_defaults(void *cfg);
void matrix_free_config(void *cfg);
Effect *matrix_make(const void *cfg);

#endif
