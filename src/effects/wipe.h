// wipe, ported from the reference effects/effect_wipe.py.
#ifndef GLYPHFX_EFFECT_WIPE_H
#define GLYPHFX_EFFECT_WIPE_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    CharacterGroup wipe_direction;
    int64_t wipe_delay;
    Easing wipe_ease;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    int64_t final_gradient_frames;
    GradientDirection final_gradient_direction;
} WipeConfig;

void wipe_config_defaults(void *cfg);
void wipe_free_config(void *cfg);
Effect *wipe_make(const void *cfg);

#endif
