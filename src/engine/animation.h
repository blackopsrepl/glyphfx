// CharacterVisual, Frame, Scene, and Animation, ported from the reference
// engine/animation.py. Scene/Animation stepping that fires events lives on the
// EngineCtx; this module is state plus event-free logic.
#ifndef GLYPHFX_ANIMATION_H
#define GLYPHFX_ANIMATION_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "utils/ansi.h"
#include "utils/easing.h"
#include "utils/graphics.h"
#include "utils/ordmap.h"

typedef enum {
    EXISTING_COLOR_ALWAYS,
    EXISTING_COLOR_DYNAMIC,
    EXISTING_COLOR_IGNORE,
} ExistingColorHandling;

typedef enum {
    SYNC_DISTANCE,
    SYNC_STEP,
} SyncMetric;

typedef struct VisualParams {
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

// A visual is a handle into the offset-addressed pool (engine/visual.h): the
// formatted bytes' pool offset and length, with a header that carries what the
// visual is. Frame and grid store the handle; nothing refcounts.
#include "engine/visual.h"

// Eased scenes pick a frame index from (ease, total_steps, step) alone, and
// every character stepping the same scene shape picks the same indices, so the
// sequence is memoized per shape instead of recomputing cos/pow and rounding in
// every character's tick. Identical to evaluating easing_ease directly.
int64_t eased_frame_index(const Easing *ease, int64_t total_steps, int64_t step);

typedef struct {
    size_t *items;
    size_t len;
    size_t cap;
    size_t head;
} IndexDeque;

void iq_init(IndexDeque *q);
void iq_free(IndexDeque *q);
void iq_push_back(IndexDeque *q, size_t v);
bool iq_pop_front(IndexDeque *q, size_t *out);
bool iq_at(const IndexDeque *q, size_t index, size_t *out);
bool iq_back(const IndexDeque *q, size_t *out);
void iq_append(IndexDeque *dst, IndexDeque *src);
static inline size_t iq_len(const IndexDeque *q) {
    return q->len;
}

typedef struct Frame {
    VisualHandle visual;
    int64_t duration;
    int64_t ticks_elapsed;
} Frame;

// Eased scenes pick a frame index from (ease, total_steps, step) alone, and
// every character stepping the same scene shape picks the same indices, so the
// sequence is memoized per shape instead of recomputing cos/pow and rounding in
// every character's tick. Identical to evaluating easing_ease directly.
int64_t eased_frame_index(const Easing *ease, int64_t total_steps, int64_t step);

typedef struct {
    char *scene_id;  // owned
    bool is_looping;
    bool no_color;
    bool use_xterm_colors;
    bool has_sync;
    SyncMetric sync;
    bool has_ease;
    Easing ease;
    Frame *all_frames;
    size_t all_frames_len;
    size_t all_frames_cap;
    // The remaining queue is just a head index into all_frames: frames retire
    // in order, reset restores the original order, and the synced/eased steps
    // index the remaining queue without reordering it. Reaching all_frames_len
    // is "no remaining frames"; a looping scene wraps the head to 0.
    size_t head;
    size_t *frame_index_map;  // tick index -> frame index
    size_t frame_index_map_len;
    size_t frame_index_map_cap;
    int64_t easing_total_steps;
    int64_t easing_current_step;
    bool has_preexisting_colors;
    ColorPair preexisting_colors;
    bool preexisting_bold;
} Scene;

struct Easing;

typedef struct {
    OrdMap scenes;  // char* -> Scene*
    char *active_scene;  // owned copy of the active scene id, or NULL
    size_t active_scene_slot;  // cached OrdMap slot for the active scene
    bool active_scene_slot_valid;
    Scene *active_scene_ref;  // borrowed; the scene active_scene names, or NULL
    bool use_xterm_colors;
    bool no_color;
    ExistingColorHandling existing_color_handling;
    char *input_symbol;  // owned
    bool has_input_fg;
    Color input_fg_color;
    bool has_input_bg;
    Color input_bg_color;
    bool input_bold;
    int64_t active_scene_current_step;
    VisualHandle current_visual;
    int32_t render_id;  // arena index, for the renderer's change log
} Animation;

void animation_init(Animation *anim, const char *input_symbol);
void animation_free(Animation *anim);
void animation_set_appearance(Animation *anim, bool uses_input_preexisting_colors, const char *symbol,
                              const ColorPair *colors);
// Returns a stable pointer to the interned scene id (NULL on failure).
const char *animation_new_scene(Animation *anim, bool is_looping, bool has_sync, SyncMetric sync, bool has_ease,
                                Easing ease, const char *scene_id, bool uses_input_preexisting_colors);
bool animation_active_scene_is_complete(const Animation *anim);
Scene *animation_active_scene(Animation *anim);
// Removes every scene (used by particle reset).
void animation_clear_scenes(Animation *anim);

// Scene operations.
void scene_free(Scene *scene);
int scene_add_frame(Scene *scene, const char *symbol, int64_t duration, const VisualParams *params);
// Appends every frame of src to dst, sharing the pooled visuals and copying
// durations; playback counters start fresh and easing bookkeeping matches
// scene_add_frame. For templates whose frames are byte-identical to what dst
// would build for itself.
int scene_append_frames(Scene *dst, const Scene *src);

// Memo of template scenes keyed by (symbol, color): effects whose frame
// content depends only on that pair build one scene and share it across
// characters (the asm engine's visual_run_find/visual_run_keep).
typedef struct FrameMemo FrameMemo;
FrameMemo *framemo_new(void);
// The template scene recorded for (symbol, fg), or NULL.
Scene *framemo_get(FrameMemo *m, const char *symbol, Color fg);
int framemo_put(FrameMemo *m, const char *symbol, Color fg, Scene *scene);
void framemo_free(FrameMemo *m);
// Returns 0 on success; sets the active frame's visual (borrowed) and its
// all_frames index.
int scene_activate(const Scene *scene, VisualHandle *out, size_t *frame_index);
void scene_get_next_visual(Scene *scene, VisualHandle *out, size_t *frame_index);
int scene_apply_gradient_to_symbols(Scene *scene, const char *const *symbols, size_t n_symbols, int64_t duration,
                                    const Gradient *fg, const Gradient *bg);
void scene_reset(Scene *scene);

#endif
