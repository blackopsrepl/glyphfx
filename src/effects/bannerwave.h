// bannerwave: a glyphfx original effect (not in ttfx; outside the parity set).
#ifndef GLYPHFX_EFFECT_BANNERWAVE_H
#define GLYPHFX_EFFECT_BANNERWAVE_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    double wave_amplitude;
    int64_t wave_length;
    int64_t wave_period;
    int64_t reveal_frames;
    int64_t wave_frames;
    int64_t settle_frames;
    Color highlight_color;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} BannerwaveConfig;

void bannerwave_config_defaults(void *cfg);
void bannerwave_free_config(void *cfg);
Effect *bannerwave_make(const void *cfg);

#endif
