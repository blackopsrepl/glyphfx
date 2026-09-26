// bouncyballs, ported from the reference effects/bouncyballs.rs.
#ifndef GLYPHFX_EFFECT_BOUNCYBALLS_H
#define GLYPHFX_EFFECT_BOUNCYBALLS_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

// Vec<String> for ball-symbols: a list of single-codepoint symbols.
typedef StringList BouncyBallsSymbolList;

typedef struct {
    ColorList ball_colors;
    BouncyBallsSymbolList ball_symbols;
    int64_t ball_delay;
    double movement_speed;
    Easing movement_easing;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} BouncyBallsConfig;

void bouncyballs_config_defaults(void *cfg);
void bouncyballs_free_config(void *cfg);
Effect *bouncyballs_make(const void *cfg);

#endif
