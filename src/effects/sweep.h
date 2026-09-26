// sweep, ported from the reference effects/sweep.rs.
#ifndef GLYPHFX_EFFECT_SWEEP_H
#define GLYPHFX_EFFECT_SWEEP_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    StringList sweep_symbols;
    CharacterGroup first_sweep_direction;
    CharacterGroup second_sweep_direction;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} SweepConfig;

void sweep_config_defaults(void *cfg);
void sweep_free_config(void *cfg);
Effect *sweep_make(const void *cfg);

#endif
