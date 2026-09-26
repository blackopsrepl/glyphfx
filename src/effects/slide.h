// slide, ported from the reference effects/slide.rs.
#ifndef GLYPHFX_EFFECT_SLIDE_H
#define GLYPHFX_EFFECT_SLIDE_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef enum {
    SLIDE_GROUP_ROW,
    SLIDE_GROUP_COLUMN,
    SLIDE_GROUP_DIAGONAL,
} SlideGrouping;

typedef struct {
    double movement_speed;
    SlideGrouping grouping;
    int64_t gap;
    bool reverse_direction;
    bool merge;
    Easing movement_easing;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    int64_t final_gradient_frames;
    GradientDirection final_gradient_direction;
} SlideConfig;

void slide_config_defaults(void *cfg);
void slide_free_config(void *cfg);
Effect *slide_make(const void *cfg);

#endif
