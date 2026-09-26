// thunderstorm, ported from the reference effects/thunderstorm.rs.
#ifndef GLYPHFX_EFFECT_THUNDERSTORM_H
#define GLYPHFX_EFFECT_THUNDERSTORM_H

#include <stddef.h>
#include <stdint.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    Color lightning_color;
    Color glowing_text_color;
    int64_t text_glow_time;
    StringList raindrop_symbols;
    StringList spark_symbols;
    Color spark_glow_color;
    int64_t spark_glow_time;
    int64_t storm_time;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    int64_t final_gradient_frames;
    GradientDirection final_gradient_direction;
} ThunderstormConfig;

void thunderstorm_config_defaults(void *cfg);
void thunderstorm_free_config(void *cfg);
Effect *thunderstorm_make(const void *cfg);

#endif
