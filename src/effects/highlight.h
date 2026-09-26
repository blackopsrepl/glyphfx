// highlight, ported from the reference effects/highlight.rs.
#ifndef GLYPHFX_EFFECT_HIGHLIGHT_H
#define GLYPHFX_EFFECT_HIGHLIGHT_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    double highlight_brightness;
    CharacterGroup highlight_direction;
    int64_t highlight_width;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} HighlightConfig;

void highlight_config_defaults(void *cfg);
void highlight_free_config(void *cfg);
Effect *highlight_make(const void *cfg);

#endif
