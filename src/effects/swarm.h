// swarm, ported from the reference effects/swarm.rs.
#ifndef GLYPHFX_EFFECT_SWARM_H
#define GLYPHFX_EFFECT_SWARM_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    ColorList base_color;
    Color flash_color;
    double swarm_size;
    double swarm_coordination;
    IntRange swarm_area_count_range;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} SwarmConfig;

void swarm_config_defaults(void *cfg);
void swarm_free_config(void *cfg);
Effect *swarm_make(const void *cfg);

#endif
