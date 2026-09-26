// CharacterVisual, Frame, Scene, and Animation, ported from the reference
// engine/animation.py. Scene/Animation stepping that fires events lives on the
// EngineCtx; this module is state plus event-free logic.
#ifndef GLYPHFX_ANIMATION_H
#define GLYPHFX_ANIMATION_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "utils/ansi.h"
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

// Opaque here; defined in utils/easing.h.
typedef struct Easing Easing;

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
void vis_move(CharacterVisual *dst, CharacterVisual *src);
void vis_format(CharacterVisual *vis);

typedef struct {
    CharacterVisual visual;
    int64_t duration;
    int64_t ticks_elapsed;
} Frame;

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
void iq_append(IndexDeque *dst, IndexDeque *src);
static inline size_t iq_len(const IndexDeque *q) {
    return q->len;
}

typedef struct {
    char *scene_id;  // owned
    bool is_looping;
    bool no_color;
    bool use_xterm_colors;
    bool has_sync;
    SyncMetric sync;
    const Easing *ease;
    Frame *all_frames;
    size_t all_frames_len;
    size_t all_frames_cap;
    IndexDeque frames;        // remaining frame indices
    IndexDeque played_frames; // played frame indices
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
    CharacterVisual current_visual;
} Animation;

void animation_init(Animation *anim, const char *input_symbol);
void animation_free(Animation *anim);
void animation_set_appearance(Animation *anim, bool uses_input_preexisting_colors, const char *symbol,
                              const ColorPair *colors);
// Returns a stable pointer to the interned scene id (NULL on failure).
const char *animation_new_scene(Animation *anim, bool is_looping, bool has_sync, SyncMetric sync,
                                const Easing *ease, const char *scene_id, bool uses_input_preexisting_colors);
bool animation_active_scene_is_complete(const Animation *anim);
Scene *animation_active_scene(Animation *anim);

// Scene operations.
void scene_free(Scene *scene);
int scene_add_frame(Scene *scene, const char *symbol, int64_t duration, const VisualParams *params);
int scene_activate(const Scene *scene, CharacterVisual *out);
void scene_get_next_visual(Scene *scene, CharacterVisual *out);
int scene_apply_gradient_to_symbols(Scene *scene, const char *const *symbols, size_t n_symbols, int64_t duration,
                                    const Gradient *fg, const Gradient *bg);
void scene_reset(Scene *scene);

#endif
