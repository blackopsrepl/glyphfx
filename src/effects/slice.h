// slice, ported from the reference effects/slice.rs.
#ifndef GLYPHFX_EFFECT_SLICE_H
#define GLYPHFX_EFFECT_SLICE_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef enum {
    SLICE_VERTICAL,
    SLICE_HORIZONTAL,
    SLICE_DIAGONAL,
} SliceDirection;

typedef struct {
    SliceDirection slice_direction;
    double movement_speed;
    Easing movement_easing;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} SliceConfig;

void slice_config_defaults(void *cfg);
void slice_free_config(void *cfg);
Effect *slice_make(const void *cfg);

#endif
