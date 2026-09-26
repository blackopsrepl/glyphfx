// decrypt, ported from the reference effects/decrypt.rs.
#ifndef GLYPHFX_EFFECT_DECRYPT_H
#define GLYPHFX_EFFECT_DECRYPT_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    int64_t typing_speed;
    ColorList ciphertext_colors;
    ColorList final_gradient_stops;
    IntList final_gradient_steps;
    GradientDirection final_gradient_direction;
} DecryptConfig;

void decrypt_config_defaults(void *cfg);
void decrypt_free_config(void *cfg);
Effect *decrypt_make(const void *cfg);

#endif
