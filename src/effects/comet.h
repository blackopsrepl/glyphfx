// comet: a glyphfx original effect (not in ttfx; outside the parity set).
#ifndef GLYPHFX_EFFECT_COMET_H
#define GLYPHFX_EFFECT_COMET_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    CharacterGroup launch_order;
    int64_t launch_delay;
    int64_t launch_jitter;
    int64_t flight_frames;
    Easing flight_ease;
    IntRange arc_height;
    int64_t trail_length;
    StringList trail_symbols;
    Color comet_color;
    int64_t cooldown_frames;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} CometConfig;

void comet_config_defaults(void *cfg);
void comet_free_config(void *cfg);
Effect *comet_make(const void *cfg);

#endif
