// binarypath, ported from the reference effects/binarypath.rs.
#ifndef GLYPHFX_EFFECT_BINARYPATH_H
#define GLYPHFX_EFFECT_BINARYPATH_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
    ColorList binary_colors;
    double movement_speed;
    double active_binary_groups;
} BinaryPathConfig;

void binarypath_config_defaults(void *cfg);
void binarypath_free_config(void *cfg);
Effect *binarypath_make(const void *cfg);

#endif
