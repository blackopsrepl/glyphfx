// randomsequence, ported from the reference effects/effect_random_sequence.py.
#ifndef GLYPHFX_EFFECT_RANDOM_SEQUENCE_H
#define GLYPHFX_EFFECT_RANDOM_SEQUENCE_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    double speed;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    int64_t final_gradient_frames;
    GradientDirection final_gradient_direction;
} RandomSequenceConfig;

extern const EffOptSpec randomsequence_specs[];
extern const size_t randomsequence_specs_len;
void randomsequence_config_defaults(void *cfg);
Effect *randomsequence_make(const void *cfg);

#endif
