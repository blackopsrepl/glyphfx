// spray, ported from the reference effects/spray.rs.
#ifndef GLYPHFX_EFFECT_SPRAY_H
#define GLYPHFX_EFFECT_SPRAY_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef enum {
    SPRAY_N,
    SPRAY_NE,
    SPRAY_E,
    SPRAY_SE,
    SPRAY_S,
    SPRAY_SW,
    SPRAY_W,
    SPRAY_NW,
    SPRAY_CENTER,
} SprayPosition;

typedef struct {
    SprayPosition spray_position;
    double spray_volume;
    FloatRange movement_speed_range;
    Easing movement_easing;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} SprayConfig;

void spray_config_defaults(void *cfg);
void spray_free_config(void *cfg);
Effect *spray_make(const void *cfg);

#endif
