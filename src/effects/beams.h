// beams, ported from the reference effects/beams.rs.
#ifndef GLYPHFX_EFFECT_BEAMS_H
#define GLYPHFX_EFFECT_BEAMS_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    StringList beam_row_symbols;
    StringList beam_column_symbols;
    int64_t beam_delay;
    IntRange beam_row_speed_range;
    IntRange beam_column_speed_range;
    ColorList beam_gradient_stops;
    IntList beam_gradient_steps;
    int64_t beam_gradient_frames;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    int64_t final_gradient_frames;
    GradientDirection final_gradient_direction;
    int64_t final_wipe_speed;
} BeamsConfig;

void beams_config_defaults(void *cfg);
void beams_free_config(void *cfg);
Effect *beams_make(const void *cfg);

#endif
