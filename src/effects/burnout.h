// burnout: a glyphfx original exit effect (not in ttfx; outside the parity set).
#ifndef GLYPHFX_EFFECT_BURNOUT_H
#define GLYPHFX_EFFECT_BURNOUT_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    int64_t hold_frames;
    CharacterGroup burn_direction;
    int64_t burn_spread;
    int64_t burn_jitter;
    int64_t burn_frames;
    StringList burn_symbols;
    ColorList ember_colors;
    double spark_ratio;
    int64_t spark_frames;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} BurnoutConfig;

void burnout_config_defaults(void *cfg);
void burnout_free_config(void *cfg);
Effect *burnout_make(const void *cfg);

#endif
