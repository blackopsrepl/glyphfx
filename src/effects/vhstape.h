// vhstape, ported from the reference effects/vhstape.rs.
#ifndef GLYPHFX_EFFECT_VHSTAPE_H
#define GLYPHFX_EFFECT_VHSTAPE_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    ColorList glitch_line_colors;
    ColorList glitch_wave_colors;
    ColorList noise_colors;
    double glitch_line_chance;
    double noise_chance;
    int64_t total_glitch_time;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} VhsTapeConfig;

void vhstape_config_defaults(void *cfg);
void vhstape_free_config(void *cfg);
Effect *vhstape_make(const void *cfg);

#endif
