// crumble, ported from the reference effects/crumble.rs.
#ifndef GLYPHFX_EFFECT_CRUMBLE_H
#define GLYPHFX_EFFECT_CRUMBLE_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} CrumbleConfig;

void crumble_config_defaults(void *cfg);
void crumble_free_config(void *cfg);
Effect *crumble_make(const void *cfg);

#endif
