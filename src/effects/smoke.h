// smoke, ported from the reference effects/smoke.rs.
#ifndef GLYPHFX_EFFECT_SMOKE_H
#define GLYPHFX_EFFECT_SMOKE_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    Color starting_color;
    StringList smoke_symbols;
    ColorList smoke_gradient_stops;
    bool use_whole_canvas;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} SmokeConfig;

void smoke_config_defaults(void *cfg);
void smoke_free_config(void *cfg);
Effect *smoke_make(const void *cfg);

#endif
