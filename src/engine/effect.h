// Effect vtable and run loop (reference engine/effect.rs equivalents).
#ifndef GLYPHFX_EFFECT_H
#define GLYPHFX_EFFECT_H

#include "engine/character.h"
#include "engine/events.h"

struct EngineCtx;

typedef struct Effect Effect;

typedef struct {
    int (*build)(Effect *self, struct EngineCtx *ctx);
    // Returns a malloc'd frame string, or NULL when the effect is finished.
    char *(*next_frame)(Effect *self, struct EngineCtx *ctx);
    void (*destroy)(Effect *self);
    void (*dispatch_callback)(Effect *self, struct EngineCtx *ctx, CharId character, const EffectCallback *cb);
} EffectOps;

struct Effect {
    const EffectOps *ops;
    void *state;
};

#endif
