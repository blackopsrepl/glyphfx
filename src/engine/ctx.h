// EngineCtx: the mutable engine world (terminal + rng + clock + active
// characters) and every stepping routine that can fire events. Event dispatch
// is synchronous and inline at the emission point and may recurse.
#ifndef GLYPHFX_CTX_H
#define GLYPHFX_CTX_H

#include <stdbool.h>

#include "engine/active_characters.h"
#include "engine/effect.h"
#include "engine/input.h"
#include "engine/terminal.h"
#include "utils/clock.h"
#include "utils/rng.h"

typedef struct EngineCtx {
    Terminal terminal;
    Rng rng;
    Clock clock;
    ActiveCharacters active_characters;
    CharId *scratch;
    size_t scratch_len;
    bool preexisting_colors_present;
} EngineCtx;

int engine_ctx_init(EngineCtx *ctx, const char *input_data, const TerminalConfig *config, Rng rng, Clock clock,
                    PreprocessError *err);
void engine_ctx_free(EngineCtx *ctx);

void engine_activate_scene(EngineCtx *ctx, Effect *effect, CharId id, const char *scene_id);
void engine_deactivate_scene(EngineCtx *ctx, CharId id, const char *scene_id);
void engine_step_animation(EngineCtx *ctx, Effect *effect, CharId id);
void engine_motion_move(EngineCtx *ctx, Effect *effect, CharId id);
void engine_tick(EngineCtx *ctx, Effect *effect, CharId id);
void engine_update(EngineCtx *ctx, Effect *effect);
char *engine_frame(EngineCtx *ctx);
void engine_handle_event(EngineCtx *ctx, Effect *effect, CharId id, Event event, const CallerKey *caller);

int effect_dump(Effect *effect, EngineCtx *ctx, bool has_max_frames, uint64_t max_frames);
int effect_run(Effect *effect, EngineCtx *ctx, bool tty_output);

#endif
