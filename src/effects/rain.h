// rain, ported from the reference effects/rain.rs.
#ifndef GLYPHFX_EFFECT_RAIN_H
#define GLYPHFX_EFFECT_RAIN_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

// Vec<String> for rain-symbols: a list of single-codepoint symbols.
typedef StringList RainSymbolList;

typedef struct {
    ColorList rain_colors;
    FloatRange movement_speed;
    RainSymbolList rain_symbols;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
    Easing movement_easing;
} RainConfig;

void rain_config_defaults(void *cfg);
void rain_free_config(void *cfg);
Effect *rain_make(const void *cfg);

#endif
