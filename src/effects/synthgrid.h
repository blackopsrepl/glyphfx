// synthgrid, ported from the reference effects/synthgrid.rs.
#ifndef GLYPHFX_EFFECT_SYNTHGRID_H
#define GLYPHFX_EFFECT_SYNTHGRID_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    ColorList grid_gradient_stops;
    IntList grid_gradient_steps;
    GradientDirection grid_gradient_direction;
    ColorList text_gradient_stops;
    IntList text_gradient_steps;
    GradientDirection text_gradient_direction;
    char *grid_row_symbol;
    char *grid_column_symbol;
    StringList text_generation_symbols;
    double max_active_blocks;
} SynthGridConfig;

void synthgrid_config_defaults(void *cfg);
void synthgrid_free_config(void *cfg);
Effect *synthgrid_make(const void *cfg);

#endif
