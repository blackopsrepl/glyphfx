// laseretch, ported from the reference effects/laseretch.rs.
#ifndef GLYPHFX_EFFECT_LASERETCH_H
#define GLYPHFX_EFFECT_LASERETCH_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef enum {
    ETCH_PATTERN_ALGORITHM = 0,
    ETCH_PATTERN_GROUP = 1,
} EtchPattern;

typedef struct {
    EtchPattern etch_pattern;
    int64_t etch_speed;
    int64_t etch_delay;
    ColorList cool_gradient_stops;
    ColorList laser_gradient_stops;
    ColorList spark_gradient_stops;
    int64_t spark_cooling_frames;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    int64_t final_gradient_frames;
    GradientDirection final_gradient_direction;
} LaserEtchConfig;

void laseretch_config_defaults(void *cfg);
void laseretch_free_config(void *cfg);
Effect *laseretch_make(const void *cfg);

#endif
