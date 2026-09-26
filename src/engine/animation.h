// CharacterVisual and Animation state, ported from the reference
// engine/animation.py. The full Scene/Frame machinery arrives in M1; M0 needs
// only the per-character current visual that the renderer reads.
#ifndef GLYPHFX_ANIMATION_H
#define GLYPHFX_ANIMATION_H

#include <stdbool.h>
#include <stdint.h>

#include "utils/ansi.h"
#include "utils/graphics.h"

typedef enum {
    EXISTING_COLOR_ALWAYS,
    EXISTING_COLOR_DYNAMIC,
    EXISTING_COLOR_IGNORE,
} ExistingColorHandling;

typedef struct {
    bool bold;
    bool dim;
    bool italic;
    bool underline;
    bool blink;
    bool reverse;
    bool hidden;
    bool strike;
    bool has_colors;
    ColorPair colors;
    bool has_fg_code;
    ColorCode fg_code;
    bool has_bg_code;
    ColorCode bg_code;
} VisualParams;

// One cell's appearance plus its precomputed ANSI string.
typedef struct {
    char *symbol;  // owned
    bool bold;
    bool dim;  // stored but never emitted, faithfully
    bool italic;
    bool underline;
    bool blink;
    bool reverse;
    bool hidden;
    bool strike;
    bool has_colors;
    ColorPair colors;
    bool has_fg_code;
    ColorCode fg_code;
    bool has_bg_code;
    ColorCode bg_code;
    char *formatted;  // owned
} CharacterVisual;

void vis_init(CharacterVisual *vis, const char *symbol, const VisualParams *params);
void vis_init_plain(CharacterVisual *vis, const char *symbol);
void vis_free(CharacterVisual *vis);
void vis_copy(CharacterVisual *dst, const CharacterVisual *src);
// Rebuilds `formatted` from the current fields.
void vis_format(CharacterVisual *vis);

typedef struct {
    char *input_symbol;  // owned; the character's original symbol
    bool has_input_fg;
    Color input_fg_color;
    bool has_input_bg;
    Color input_bg_color;
    bool input_bold;
    bool use_xterm_colors;
    bool no_color;
    ExistingColorHandling existing_color_handling;
    CharacterVisual current_visual;
} Animation;

void animation_init(Animation *anim, const char *input_symbol);
void animation_free(Animation *anim);
// Animation.set_appearance: symbol NULL means use the input symbol; colors
// NULL means empty ColorPair.
void animation_set_appearance(Animation *anim, bool uses_input_preexisting_colors, const char *symbol,
                              const ColorPair *colors);

#endif
