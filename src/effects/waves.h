// waves, ported from the reference effects/waves.rs.
#ifndef GLYPHFX_EFFECT_WAVES_H
#define GLYPHFX_EFFECT_WAVES_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    StringList wave_symbols;
    ColorList wave_gradient_stops;
    IntList wave_gradient_steps;
    int64_t wave_count;
    int64_t wave_length;
    CharacterGroup wave_direction;
    Easing wave_easing;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} WavesConfig;

void waves_config_defaults(void *cfg);
void waves_free_config(void *cfg);
Effect *waves_make(const void *cfg);

#endif
