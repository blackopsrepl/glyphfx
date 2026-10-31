// EngineCtx: the mutable engine world (terminal + rng + clock + active
// characters) and every stepping routine that can fire events. Event dispatch
// is synchronous and inline at the emission point and may recurse.
#ifndef GLYPHFX_CTX_H
#define GLYPHFX_CTX_H

#include <stdbool.h>

#include "engine/active_characters.h"
#include "engine/doze.h"
#include "engine/effect.h"
#include "engine/input.h"
#include "engine/motion.h"
#include "engine/terminal.h"
#include "utils/clock.h"
#include "utils/rng.h"

// Pure-segment mirrors, indexed by character slot. The word pass evaluates
// these without traversing characters, maps or frame records; ordered ticks
// validate the mirror before committing its speculative result.
typedef struct MotionEaseTable {
    struct MotionEaseTable *next;
    Easing ease;
    int64_t steps;
    double factors[];
} MotionEaseTable;

typedef struct {
    Path *path;  // resolved active path, NULL when there is none
    int64_t step;       // the path's current_step
    uint64_t identity, geometry;  // path identity and revision at capture
    size_t segment;
    size_t segments_len;
    int64_t max_steps;
    double total, offset, limit;
    Coord start, control, end;
    Easing ease;
    const double *factors;
    bool has_ease, curved, over, valid;
} MotionMirror;

// Structure-of-arrays input for one bitmap word. Capture writes geometry once;
// the SIMD kernel loads four adjacent slots from each numeric stream.
typedef struct {
    double step[64], max[64], total[64], offset[64], limit[64];
    double sx[64], sy[64], cx[64], cy[64], ex[64], ey[64];
} MotionWord;

// One bitmap word's speculative results. Preparing a word never changes Path
// records; callbacks can discard all remaining lanes without rolling back state.
typedef struct {
    Coord coord[64];
    double distance[64];
    int64_t step[64];
    Scene *scene[64];
    size_t scene_head[64], scene_count[64];
    VisualHandle visual[64];
    unsigned scene_flags[64];
    uint64_t ready, idle, animation, epoch;
    size_t word;
} MotionBatch;

typedef struct EngineCtx {
    Terminal terminal;
    Rng rng;
    Clock clock;
    ActiveCharacters active_characters;
    Doze doze;  // characters parked through their pure ticks (engine/doze.h)
    MotionMirror *mirrors;  // one per arena slot, grown with the arena
    size_t mirrors_cap;
    MotionWord *motion_words;
    uint64_t *motion_mirrored;  // reusable mirror slots, one bit per character
    uint64_t motion_epoch;  // callbacks invalidate speculative motion
    MotionBatch motion_batch;
    MotionEaseTable *motion_eases;
    size_t motion_ease_values;
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
// Makes ctx the context whose dozes the engine's context-free hooks (scene and
// active-set calls, engine/doze.h) end; NULL unbinds. engine_update binds its
// own context. A host that interleaves several contexts in one process binds
// each one before calling into it (the Rust bridge does, in bind()).
void engine_bind(EngineCtx *ctx);
const char *engine_frame(EngineCtx *ctx);
void engine_handle_event(EngineCtx *ctx, Effect *effect, CharId id, Event event, const CallerKey *caller);
void engine_activate_path(EngineCtx *ctx, Effect *effect, CharId id, const char *path_id);
// Builds the waypoint caller key used to register/match segment events.
void engine_caller_from_waypoint(const Waypoint *wp, CallerKey *out);
// Returns 0 on success, -1 on duplicate registration.
int engine_register_event(EngineCtx *ctx, CharId id, Event event, const CallerKey *caller, const EventAction *action);
int engine_chain_paths(EngineCtx *ctx, CharId id, const char *const *paths, size_t n_paths, bool loop);

int effect_dump(Effect *effect, EngineCtx *ctx, bool has_max_frames, uint64_t max_frames);
int effect_run(Effect *effect, EngineCtx *ctx, bool tty_output, RunOutcome *out_outcome);

#endif
