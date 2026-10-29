// Shared helpers for glyphfx's original effects (comet, copper, bannerwave,
// burnout). These effects have no counterpart in ttfx, so they are outside the
// byte-parity set: each computes every character's appearance and position as
// a pure function of the frame number and drives the engine directly, which
// keeps them deterministic for a given input, option set and seed.
#ifndef GLYPHFX_EFFECT_ORIGINALS_H
#define GLYPHFX_EFFECT_ORIGINALS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cli.h"
#include "engine/ctx.h"
#include "utils/graphics.h"

// Colour mixes are snapped to this many steps so the interned visual pool stays
// bounded however long an effect runs.
#define ORIG_MIX_STEPS 16

// Every input character with its home coordinate and final colour.
typedef struct {
    CharId *ids;
    Color *final;
    bool *has_final;  // false only under dynamic colour handling without an input colour
    size_t len;
} OrigCast;

// Collects the input characters in reading order and assigns their final
// colours: the final gradient across the text, or the input colours under
// --existing-color-handling dynamic (the convention of the reference effects).
int orig_cast_build(EngineCtx *ctx, const ColorList *stops, const IntList *steps, GradientDirection direction,
                    OrigCast *out);
void orig_cast_free(OrigCast *cast);

double orig_clamp01(double x);
double orig_smooth(double x);
Color orig_mix(const Color *a, const Color *b, double t);
// Samples a multi-stop ramp at t in [0, 1].
Color orig_ramp(const Color *stops, size_t n, double t);

// Sets the symbol and colours; NULL fg or bg leaves that channel unset.
void orig_look(EngineCtx *ctx, CharId id, const char *symbol, const Color *fg, const Color *bg);
// Moves the character and shows it when the coordinate is on the canvas,
// hides it otherwise.
void orig_place(EngineCtx *ctx, CharId id, Coord coord);
void orig_hide(EngineCtx *ctx, CharId id);
void orig_layer(EngineCtx *ctx, CharId id, int64_t layer);

void orig_push_color(ColorList *list, const char *hex);
void orig_push_int(IntList *list, int64_t v);
void orig_push_symbol(StringList *list, const char *symbol);
void orig_free_symbols(StringList *list);

#endif
