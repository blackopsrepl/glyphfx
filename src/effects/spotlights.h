// spotlights, ported from the reference effects/spotlights.rs.
#ifndef GLYPHFX_EFFECT_SPOTLIGHTS_H
#define GLYPHFX_EFFECT_SPOTLIGHTS_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    double beam_width_ratio;
    double beam_falloff;
    int64_t search_duration;
    FloatRange search_speed_range;
    int64_t spotlight_count;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} SpotlightsConfig;

void spotlights_config_defaults(void *cfg);
void spotlights_free_config(void *cfg);
Effect *spotlights_make(const void *cfg);

#endif
