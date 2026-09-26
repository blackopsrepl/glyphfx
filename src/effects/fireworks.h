// fireworks, ported from the reference effects/fireworks.rs.
#ifndef GLYPHFX_EFFECT_FIREWORKS_H
#define GLYPHFX_EFFECT_FIREWORKS_H

#include <stdbool.h>
#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    bool explode_anywhere;
    ColorList firework_colors;
    char *firework_symbol;
    double firework_volume;
    int64_t launch_delay;
    double explode_distance;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} FireworksConfig;

void fireworks_config_defaults(void *cfg);
void fireworks_free_config(void *cfg);
Effect *fireworks_make(const void *cfg);

#endif
