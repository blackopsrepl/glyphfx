#include "engine/animation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__linux__)
#include <sys/mman.h>
#endif

#include "utils/hexterm.h"
#include "engine/doze.h"
#include "engine/render_log.h"
#include "utils/strbuf.h"
#include "utils/strhash.h"
#include "utils/pycompat.h"
#include "utils/strtab.h"

static char *dup_cstr(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, len + 1);
    return out;
}

static bool resolve_color_code(bool has_color, const Color *color, bool no_color, bool use_xterm_colors,
                               ColorCode *out) {
    if (!has_color || no_color) {
        return false;
    }
    if (use_xterm_colors) {
        uint8_t code = color->is_xterm ? color->xterm : hex_to_xterm(color->hex);
        *out = colorcode_xterm(code);
    } else {
        *out = colorcode_rgb(color->hex);
    }
    return true;
}

// --- Scene ----------------------------------------------------------------

// Scene records are pooled too, in a region that never moves: a tick dereferences
// the scene of every character it ticks, so keeping the records adjacent is worth
// more than freeing each one.
static Scene *g_scenes;
static SceneCold *g_scene_cold;
static size_t g_scenes_len;
static size_t g_scenes_cap;

#define SCENE_POOL_RESERVE ((size_t)1 << 28)  // 256 MiB of address space

static Scene *scene_pool_alloc(void) {
    if (!g_scenes) {
        size_t bytes = SCENE_POOL_RESERVE;
#if defined(__linux__)
        void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            return NULL;
        }
        g_scenes = (Scene *)p;
#else
        g_scenes = (Scene *)calloc(1, bytes);
        if (!g_scenes) {
            return NULL;
        }
#endif
        // Keep both regions in the original reservation budget. Pad the
        // boundary explicitly rather than relying on the two types' alignment.
        size_t align = _Alignof(SceneCold);
        g_scenes_cap = (bytes - align) / (sizeof(Scene) + sizeof(SceneCold));
        size_t offset = (g_scenes_cap * sizeof(Scene) + align - 1) / align * align;
        g_scene_cold = (SceneCold *)((unsigned char *)g_scenes + offset);
    }
    if (g_scenes_len == g_scenes_cap) {
        return NULL;
    }
    Scene *scene = &g_scenes[g_scenes_len];
    scene->cold = &g_scene_cold[g_scenes_len++];
    return scene;
}

static Scene *scene_new(const char *scene_id, bool is_looping, bool has_sync, SyncMetric sync, bool has_ease,
                        Easing ease, bool no_color, bool use_xterm_colors) {
    Scene *scene = scene_pool_alloc();
    if (!scene) {
        return NULL;
    }
    scene->cold->scene_id = dup_cstr(scene_id);
    scene->is_looping = is_looping;
    scene->cold->no_color = no_color;
    scene->cold->use_xterm_colors = use_xterm_colors;
    scene->has_sync = has_sync;
    scene->sync = sync;
    scene->has_ease = has_ease;
    scene->cold->ease = ease;
    scene->head = 0;
    scene->cold->owner = -1;
    return scene;
}

void scene_wake(const Scene *scene) {
    if (scene->cold->owner >= 0) {
        doze_wake((CharId)scene->cold->owner);
    }
}

void scene_free(Scene *scene) {
    if (!scene) {
        return;
    }
    // Frames and the record itself belong to the engine's pools; the owned
    // strings are released here.
    free(scene->cold->scene_id);
}

// --- frame pool -----------------------------------------------------------
// Frames live in one reserved region instead of one malloc'd array per scene:
// the reservation never moves, so a scene's all_frames pointer stays valid, and
// the frames of neighbouring characters end up next to each other. A scene that
// needs to grow is copied to the tail of the region first (as the reference
// engine does), which keeps every region contiguous.
static Frame *g_frames;
static size_t g_frames_len;
static size_t g_frames_cap;

#define FRAME_POOL_RESERVE ((size_t)1 << 28)  // 256 MiB of address space

static bool frame_pool_init(void) {
    if (g_frames) {
        return true;
    }
    // A reservation that never moves: mmap on Linux (the Makefile's
    // _POSIX_C_SOURCE hides MAP_ANONYMOUS elsewhere), one malloc elsewhere. The
    // pool is never reallocated, so a scene's pointer into it stays valid.
    size_t bytes = FRAME_POOL_RESERVE;
#if defined(__linux__)
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        return false;
    }
    g_frames = (Frame *)p;
#else
    g_frames = (Frame *)malloc(bytes);
    if (!g_frames) {
        return false;
    }
#endif
    g_frames_cap = bytes / sizeof(Frame);
    return true;
}

void frame_pool_free(void) {
#if defined(__linux__)
    if (g_frames) {
        munmap(g_frames, FRAME_POOL_RESERVE);
    }
#else
    free(g_frames);
#endif
    g_frames = NULL;
    g_frames_len = 0;
    g_frames_cap = 0;
}

// Room for `add` more frames at the scene's tail, which must be contiguous.
static bool frame_pool_grow(Scene *scene, size_t add) {
    if (!frame_pool_init()) {
        return false;
    }
    bool at_tail = scene->all_frames != NULL &&
                   (size_t)(scene->all_frames - g_frames) + scene->all_frames_len == g_frames_len;
    if (!at_tail) {
        size_t start = g_frames_len;
        if (start + scene->all_frames_len + add > g_frames_cap) {
            return false;
        }
        if (scene->all_frames_len) {
            memcpy(g_frames + start, scene->all_frames, scene->all_frames_len * sizeof(Frame));
        }
        scene->all_frames = g_frames + start;
        g_frames_len = start + scene->all_frames_len;
    }
    if (g_frames_len + add > g_frames_cap) {
        return false;
    }
    g_frames_len += add;
    return true;
}

int scene_add_frame(Scene *scene, const char *symbol, int64_t duration, const VisualParams *params_in) {
    scene_wake(scene);
    VisualParams params = *params_in;
    if (scene->cold->has_preexisting_colors) {
        params.colors = scene->cold->preexisting_colors;
        params.has_colors = true;
    }
    if (scene->cold->preexisting_bold) {
        params.bold = true;
    }
    bool cached_appearance = params.has_colors && !params.dim && !params.italic &&
                             !params.underline && !params.blink && !params.reverse &&
                             !params.hidden && !params.strike;
    if (cached_appearance) {
        // The appearance cache interns this look and resolves its terminal
        // codes; keep the params key-equivalent and let it build the visual.
        params.has_fg_code = false;
        params.has_bg_code = false;
    } else if (params.has_colors) {
        params.has_fg_code =
            resolve_color_code(params.colors.has_fg, &params.colors.fg, scene->cold->no_color, scene->cold->use_xterm_colors,
                               &params.fg_code);
        params.has_bg_code =
            resolve_color_code(params.colors.has_bg, &params.colors.bg, scene->cold->no_color, scene->cold->use_xterm_colors,
                               &params.bg_code);
    } else {
        params.has_fg_code = false;
        params.has_bg_code = false;
    }
    if (duration < 1 || duration > INT64_MAX - scene->cold->easing_total_steps) {
        return -1;
    }
    if (!frame_pool_grow(scene, 1)) {
        return -1;
    }
    size_t frame_index = scene->all_frames_len++;
    // frame_pool_grow reserved this new slot; existing shared frames stay const.
    Frame *frame = (Frame *)&scene->all_frames[frame_index];
    memset(frame, 0, sizeof(*frame));
    frame->visual = cached_appearance
        ? visual_make_appearance(symbol, &params.colors, params.bold, scene->cold->no_color,
                                 scene->cold->use_xterm_colors)
        : visual_make(symbol, &params);
    frame->duration = duration;
    scene->cold->easing_shape_for_len = 0;  // a later eased lookup re-resolves
    scene->cold->easing_total_steps += duration;
    return 0;
}

int scene_append_frames(Scene *dst, const Scene *src) {
    scene_wake(dst);
    // Snapshot lengths before appending, including when a scene appends itself.
    size_t count = src->all_frames_len;
    if (!count) return 0;
    const Frame *frames = src->all_frames;
    size_t ticks = 0;
    for (size_t i = 0; i < count; i++) {
        if (frames[i].duration < 1 || (uint64_t)frames[i].duration > SIZE_MAX - ticks) return -1;
        ticks += (size_t)frames[i].duration;
    }
    if (count > SIZE_MAX - dst->all_frames_len ||
        ticks > (uint64_t)(INT64_MAX - dst->cold->easing_total_steps)) return -1;
    size_t base = dst->all_frames_len;
    if (!base) {
        // A frame description has no playback counter now. Sharing an immutable
        // prefix is safe even if either scene later appends: extending at the
        // pool tail cannot change the other's length or existing frame values.
        dst->all_frames = src->all_frames;
    } else {
        if (!frame_pool_grow(dst, count)) return -1;
        memcpy((Frame *)dst->all_frames + base, frames, count * sizeof(Frame));
    }
    dst->all_frames_len = base + count;
    dst->cold->easing_shape_for_len = 0;  // a later eased lookup re-resolves
    dst->cold->easing_total_steps += (int64_t)ticks;
    return 0;
}

FrameRun scene_frame_run(const Scene *scene) {
    return (FrameRun){scene->all_frames, scene->all_frames_len, scene->cold->easing_total_steps};
}

int frame_run_slice(FrameRun run, size_t first, size_t count, FrameRun *out) {
    if (first > run.len || count > run.len - first || (count && !run.frames)) return -1;
    int64_t ticks = 0;
    for (size_t i = 0; i < count; i++) {
        int64_t duration = run.frames[first + i].duration;
        if (duration < 1 || duration > INT64_MAX - ticks) return -1;
        ticks += duration;
    }
    *out = (FrameRun){run.frames ? run.frames + first : NULL, count, ticks};
    return 0;
}

int scene_set_frame_run(Scene *scene, FrameRun run) {
    if (run.total_steps < 0 || (run.len && (!run.frames || run.total_steps < 1)) ||
        (!run.len && run.total_steps)) return -1;
    scene_wake(scene);
    scene->all_frames = run.frames;
    scene->all_frames_len = run.len;
    scene->head = 0;
    scene->ticks_elapsed = 0;
    scene->easing_current_step = 0;
    scene->cold->easing_total_steps = run.total_steps;
    scene->cold->easing_shape_seq = NULL;
    scene->cold->easing_shape_for_len = 0;
    return 0;
}

// --- IndexDeque -----------------------------------------------------------

void iq_init(IndexDeque *q) {
    q->items = NULL;
    q->len = 0;
    q->cap = 0;
    q->head = 0;
}

void iq_free(IndexDeque *q) {
    free(q->items);
    iq_init(q);
}

static void iq_compact(IndexDeque *q) {
    if (q->head > 0) {
        memmove(q->items, q->items + q->head, q->len * sizeof(size_t));
        q->head = 0;
    }
}

void iq_push_back(IndexDeque *q, size_t v) {
    if (q->head + q->len == q->cap) {
        if (q->head > 0) {
            iq_compact(q);
        } else {
            size_t cap = q->cap ? q->cap * 2 : 16;
            size_t *grown = realloc(q->items, cap * sizeof(size_t));
            if (!grown) {
                return;
            }
            q->items = grown;
            q->cap = cap;
        }
    }
    q->items[q->head + q->len] = v;
    q->len++;
}

bool iq_pop_front(IndexDeque *q, size_t *out) {
    if (q->len == 0) {
        return false;
    }
    *out = q->items[q->head];
    q->head++;
    q->len--;
    if (q->len == 0) {
        q->head = 0;
    }
    return true;
}


bool iq_at(const IndexDeque *q, size_t index, size_t *out) {
    if (index >= q->len) {
        return false;
    }
    *out = q->items[q->head + index];
    return true;
}

bool iq_back(const IndexDeque *q, size_t *out) {
    if (q->len == 0) {
        return false;
    }
    *out = q->items[q->head + q->len - 1];
    return true;
}

void iq_append(IndexDeque *dst, IndexDeque *src) {
    size_t v;
    while (iq_pop_front(src, &v)) {
        iq_push_back(dst, v);
    }
}


int scene_activate(const Scene *scene, VisualHandle *out, size_t *frame_index) {
    if (scene->head >= scene->all_frames_len) {
        *out = 0;
        *frame_index = 0;
        return -1;
    }
    *out = scene->all_frames[scene->head].visual;
    *frame_index = scene->head;
    return 0;
}

void scene_get_next_visual(Scene *scene, VisualHandle *out, size_t *frame_index) {
    size_t head = scene->head;
    if (head >= scene->all_frames_len) {
        *out = 0;
        *frame_index = 0;
        return;
    }
    *out = scene->all_frames[head].visual;
    *frame_index = head;
    scene->ticks_elapsed += 1;
    if (scene->ticks_elapsed == scene->all_frames[head].duration) {
        scene->ticks_elapsed = 0;
        scene->head = head + 1;
        if (scene->is_looping && scene->head >= scene->all_frames_len) {
            scene->head = 0;
        }
    }
}

bool scene_frame_at_tick(const Scene *scene, int64_t tick, size_t *frame, int64_t *start) {
    if (tick < 0 || tick >= scene->cold->easing_total_steps || !scene->all_frames_len) return false;
    size_t at = *frame;
    int64_t first = *start;
    if (at >= scene->all_frames_len) {
        at = 0;
        first = 0;
    }
    // Easing may run backwards or overshoot; retain the walk's exact interval
    // boundaries rather than assuming that eased indices increase monotonically.
    while (tick < first && at) {
        first -= scene->all_frames[--at].duration;
    }
    while (tick - first >= scene->all_frames[at].duration) {
        first += scene->all_frames[at].duration;
        if (++at == scene->all_frames_len) return false;
    }
    *frame = at;
    *start = first;
    return true;
}

void scene_reset(Scene *scene) {
    scene_wake(scene);
    scene->head = 0;
    scene->ticks_elapsed = 0;
    scene->easing_current_step = 0;
}

static size_t count_codepoints(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

int scene_apply_gradient_to_symbols(Scene *scene, const char *const *symbols, size_t n_symbols, int64_t duration,
                                    const Gradient *fg, const Gradient *bg) {
    bool fg_has = fg && fg->len > 0;
    bool bg_has = bg && bg->len > 0;
    if (!fg && !bg) {
        return -1;
    }
    if (!fg_has && !bg_has) {
        return -1;
    }
    for (size_t i = 0; i < n_symbols; i++) {
        if (count_codepoints(symbols[i]) > 1) {
            return -1;
        }
    }

    ColorPair *pairs = NULL;
    size_t pairs_len = 0;
    if (fg_has && bg_has) {
        const Color *larger = fg->len >= bg->len ? fg->spectrum : bg->spectrum;
        const Color *smaller = fg->len >= bg->len ? bg->spectrum : fg->spectrum;
        size_t larger_len = fg->len >= bg->len ? fg->len : bg->len;
        size_t smaller_len = fg->len >= bg->len ? bg->len : fg->len;
        bool fg_larger = fg->len >= bg->len;
        pairs = malloc(larger_len * sizeof(ColorPair));
        pairs_len = larger_len;
        size_t repeat_factor = smaller_len ? larger_len / smaller_len : 0;
        size_t overflow_count = smaller_len ? larger_len % smaller_len : 0;
        bool overflow_used = false;
        size_t smaller_index = 0;
        size_t current_repeat_factor = 0;
        for (size_t i = 0; i < larger_len; i++) {
            if (current_repeat_factor >= repeat_factor) {
                if (overflow_count > 0) {
                    if (overflow_used) {
                        smaller_index++;
                        current_repeat_factor = 0;
                        overflow_used = false;
                    } else {
                        overflow_used = true;
                        overflow_count--;
                    }
                } else {
                    smaller_index++;
                    current_repeat_factor = 0;
                }
            }
            current_repeat_factor++;
            if (smaller_index >= smaller_len) {
                smaller_index = smaller_len ? smaller_len - 1 : 0;
            }
            ColorPair cp;
            memset(&cp, 0, sizeof(cp));
            if (fg_larger) {
                cp.has_fg = true;
                cp.fg = larger[i];
                cp.has_bg = true;
                cp.bg = smaller[smaller_index];
            } else {
                cp.has_fg = true;
                cp.fg = smaller[smaller_index];
                cp.has_bg = true;
                cp.bg = larger[i];
            }
            pairs[i] = cp;
        }
    } else if (fg_has) {
        pairs = malloc(fg->len * sizeof(ColorPair));
        pairs_len = fg->len;
        for (size_t i = 0; i < fg->len; i++) {
            ColorPair cp;
            memset(&cp, 0, sizeof(cp));
            cp.has_fg = true;
            cp.fg = fg->spectrum[i];
            pairs[i] = cp;
        }
    } else {
        pairs = malloc(bg->len * sizeof(ColorPair));
        pairs_len = bg->len;
        for (size_t i = 0; i < bg->len; i++) {
            ColorPair cp;
            memset(&cp, 0, sizeof(cp));
            cp.has_bg = true;
            cp.bg = bg->spectrum[i];
            pairs[i] = cp;
        }
    }

    int rc = 0;
    size_t repeat_factor = pairs_len ? n_symbols / pairs_len : 0;
    size_t overflow_count = pairs_len ? n_symbols % pairs_len : 0;
    bool overflow_used = false;
    size_t smaller_index = 0;
    size_t current_repeat_factor = 0;
    if (n_symbols >= pairs_len) {
        for (size_t i = 0; i < n_symbols; i++) {
            if (current_repeat_factor >= repeat_factor) {
                if (overflow_count > 0) {
                    if (overflow_used) {
                        smaller_index++;
                        current_repeat_factor = 0;
                        overflow_used = false;
                    } else {
                        overflow_used = true;
                        overflow_count--;
                    }
                } else {
                    smaller_index++;
                    current_repeat_factor = 0;
                }
            }
            current_repeat_factor++;
            if (pairs_len && smaller_index >= pairs_len) {
                smaller_index = pairs_len - 1;
            }
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors = pairs[smaller_index];
            if (scene_add_frame(scene, symbols[i], duration, &vp) != 0) {
                rc = -1;
                break;
            }
        }
    } else {
        size_t repeat_factor2 = n_symbols ? pairs_len / n_symbols : 0;
        size_t overflow_count2 = n_symbols ? pairs_len % n_symbols : 0;
        bool overflow_used2 = false;
        size_t smaller_index2 = 0;
        size_t current_repeat_factor2 = 0;
        for (size_t i = 0; i < pairs_len; i++) {
            if (current_repeat_factor2 >= repeat_factor2) {
                if (overflow_count2 > 0) {
                    if (overflow_used2) {
                        smaller_index2++;
                        current_repeat_factor2 = 0;
                        overflow_used2 = false;
                    } else {
                        overflow_used2 = true;
                        overflow_count2--;
                    }
                } else {
                    smaller_index2++;
                    current_repeat_factor2 = 0;
                }
            }
            current_repeat_factor2++;
            if (n_symbols && smaller_index2 >= n_symbols) {
                smaller_index2 = n_symbols - 1;
            }
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors = pairs[i];
            if (scene_add_frame(scene, symbols[smaller_index2], duration, &vp) != 0) {
                rc = -1;
                break;
            }
        }
    }
    free(pairs);
    return rc;
}

// --- Animation ------------------------------------------------------------

// --- eased frame-index memo -----------------------------------------------
// step_eased_scene evaluates an easing curve and rounds to a frame index once
// per character per tick. Both the curve and the rounding depend only on the
// scene's easing and step count, so one table per distinct (ease, total_steps)
// serves every character and removes the libm call from the hot path.
typedef struct {
    Easing ease;
    int64_t total;
    int32_t *vals;
    size_t len;
    size_t cap;
} EasedSeq;

static EasedSeq *g_eseq;
static size_t g_eseq_len;
static size_t g_eseq_cap;

static int32_t eased_compute(const Easing *e, int64_t total, int64_t step) {
    int64_t final = total > 1 ? total - 1 : 0;
    double ratio = (double)step / (double)total;
    int64_t idx = py_round_half_even(easing_ease(e, ratio) * (double)final);
    if (idx > final) {
        idx = final;
    }
    if (idx < 0) {
        idx = 0;
    }
    return (int32_t)idx;
}

static EasedSeq *eseq_find(const Easing *ease, int64_t total_steps) {
    for (size_t i = 0; i < g_eseq_len; i++) {
        if (g_eseq[i].total == total_steps && g_eseq[i].ease.kind == ease->kind &&
            g_eseq[i].ease.x1 == ease->x1 && g_eseq[i].ease.y1 == ease->y1 &&
            g_eseq[i].ease.x2 == ease->x2 && g_eseq[i].ease.y2 == ease->y2) {
            return &g_eseq[i];
        }
    }
    if (g_eseq_len == g_eseq_cap) {
        size_t cap = g_eseq_cap ? g_eseq_cap * 2 : 8;
        EasedSeq *grown = realloc(g_eseq, cap * sizeof(*grown));
        if (!grown) {
            return NULL;
        }
        g_eseq = grown;
        g_eseq_cap = cap;
    }
    EasedSeq *seq = &g_eseq[g_eseq_len++];
    memset(seq, 0, sizeof(*seq));
    seq->ease = *ease;
    seq->total = total_steps;
    return seq;
}

// Extends the memo through step want-1; false on allocation failure.
static bool eseq_fill(EasedSeq *seq, size_t want) {
    if (want <= seq->len) {
        return true;
    }
    if (want > seq->cap) {
        size_t cap = seq->cap ? seq->cap : 16;
        while (cap < want) {
            cap *= 2;
        }
        int32_t *grown = realloc(seq->vals, cap * sizeof(*grown));
        if (!grown) {
            return false;
        }
        seq->vals = grown;
        seq->cap = cap;
    }
    for (size_t i = seq->len; i < want; i++) {
        seq->vals[i] = eased_compute(&seq->ease, seq->total, (int64_t)i);
    }
    seq->len = want;
    return true;
}

// A shape resolves its FULL eased sequence once: per step, the frame whose
// tick interval the eased index falls in. Scenes built from the same frames
// (ease, total_steps, durations) share one entry, so no per-scene tick->frame
// structure is needed. fx does the same through its shape table; wave scenes
// (one easing pattern shared by a canvas of characters) are the point.
const size_t SHAPE_LIMIT = 64;
const int64_t SHAPE_MAX_STEPS = 1 << 16;

typedef struct {
    Easing ease;
    int64_t total;
    size_t frames_len;
    int64_t *durations;  // shape key: the frames' (immutable) durations
    int32_t *steps;      // shape value: frame index per step, len == total
    size_t len;          // steps resolved so far
} EasedShape;

static EasedShape *g_shapes;
static size_t g_shapes_len;
static size_t g_shapes_cap;

static bool shape_equal(const EasedShape *s, const Easing *ease, int64_t total,
                        const Frame *frames, size_t frames_len) {
    if (s->total != total || s->frames_len != frames_len) return false;
    if (s->ease.kind != ease->kind || s->ease.x1 != ease->x1 || s->ease.y1 != ease->y1 ||
        s->ease.x2 != ease->x2 || s->ease.y2 != ease->y2) return false;
    for (size_t i = 0; i < frames_len; i++) {
        if (s->durations[i] != frames[i].duration) return false;
    }
    return true;
}

// Frame whose tick interval contains `tick` in a duration list; false when
// `tick` is outside the list's total.
static bool shape_walk_tick(const Frame *frames, size_t frames_len, int64_t tick, int32_t *out) {
    int64_t first = 0;
    for (size_t i = 0; i < frames_len; i++) {
        if (tick - first < frames[i].duration) {
            *out = (int32_t)i;
            return true;
        }
        first += frames[i].duration;
    }
    return false;
}

static bool shape_extend(EasedShape *shape, size_t want, const Frame *frames) {
    if (want <= shape->len) {
        return true;
    }
    // The eased tick memo doubles as the shape's tick stream.
    const int32_t *ticks = eased_frame_seq(&shape->ease, shape->total);
    if (!ticks) {
        return false;
    }
    for (size_t i = shape->len; i < want; i++) {
        int32_t frame = -1;
        if (!shape_walk_tick(frames, shape->frames_len, ticks[i], &frame)) return false;
        shape->steps[i] = frame;
    }
    shape->len = want;
    return true;
}

const int32_t *shape_frames_by_step(const Easing *ease, int64_t total_steps, const Frame *frames,
                                    size_t frames_len) {
    if (total_steps <= 0 || total_steps > SHAPE_MAX_STEPS || frames_len == 0) return NULL;
    for (size_t i = 0; i < g_shapes_len; i++) {
        if (shape_equal(&g_shapes[i], ease, total_steps, frames, frames_len)) {
            return shape_extend(&g_shapes[i], (size_t)total_steps, frames) ? g_shapes[i].steps : NULL;
        }
    }
    if (g_shapes_len == SHAPE_LIMIT) return NULL;
    if (g_shapes_len == g_shapes_cap) {
        size_t cap = g_shapes_cap ? g_shapes_cap * 2 : 8;
        EasedShape *grown = realloc(g_shapes, cap * sizeof(*grown));
        if (!grown) return NULL;
        g_shapes = grown;
        g_shapes_cap = cap;
    }
    int64_t *durations = malloc(frames_len * sizeof(*durations));
    if (!durations) return NULL;
    int32_t *steps = calloc((size_t)total_steps, sizeof(*steps));
    if (!steps) {
        free(durations);
        return NULL;
    }
    EasedShape *shape = &g_shapes[g_shapes_len++];
    *shape = (EasedShape){.ease = *ease, .total = total_steps, .frames_len = frames_len,
                          .durations = durations, .steps = steps, .len = 0};
    for (size_t i = 0; i < frames_len; i++) {
        durations[i] = frames[i].duration;
    }
    if (!shape_extend(shape, (size_t)total_steps, frames)) {
        g_shapes_len--;
        free(durations);
        free(steps);
        return NULL;
    }
    return steps;
}

const int32_t *scene_easing_shape_seq(Scene *scene) {
    SceneCold *cold = scene->cold;
    if (cold->easing_shape_for_len == scene->all_frames_len &&
        cold->easing_shape_for_ease.kind == cold->ease.kind &&
        cold->easing_shape_for_ease.x1 == cold->ease.x1 &&
        cold->easing_shape_for_ease.y1 == cold->ease.y1 &&
        cold->easing_shape_for_ease.x2 == cold->ease.x2 &&
        cold->easing_shape_for_ease.y2 == cold->ease.y2) {
        return cold->easing_shape_seq;
    }
    cold->easing_shape_seq = NULL;
    cold->easing_shape_for_len = 0;
    if (!scene->has_ease || !scene->all_frames_len) return NULL;
    const Frame *frames = (const Frame *)scene->all_frames;
    cold->easing_shape_seq =
        shape_frames_by_step(&cold->ease, cold->easing_total_steps, frames, scene->all_frames_len);
    if (!cold->easing_shape_seq) return NULL;
    cold->easing_shape_for_len = scene->all_frames_len;
    cold->easing_shape_for_ease = cold->ease;
    return cold->easing_shape_seq;
}

int64_t eased_frame_index(const Easing *ease, int64_t total_steps, int64_t step) {
    if (total_steps <= 0 || step < 0) {
        return 0;
    }
    EasedSeq *seq = eseq_find(ease, total_steps);
    if (!seq || !eseq_fill(seq, (size_t)step + 1)) {
        return eased_compute(ease, total_steps, step);
    }
    return seq->vals[step];
}

const int32_t *eased_frame_seq(const Easing *ease, int64_t total_steps) {
    if (total_steps <= 0) {
        return NULL;
    }
    EasedSeq *seq = eseq_find(ease, total_steps);
    if (!seq || !eseq_fill(seq, (size_t)total_steps)) {
        return NULL;
    }
    return seq->vals;
}


void animation_init(Animation *anim, const char *input_symbol) {
    memset(anim, 0, sizeof(*anim));
    om_init(&anim->scenes);
    anim->input_symbol = dup_cstr(input_symbol);
    anim->existing_color_handling = EXISTING_COLOR_IGNORE;
    anim->current_visual = visual_make(input_symbol, &(VisualParams){0});
}

void animation_free(Animation *anim) {
    for (size_t i = 0; i < om_len(&anim->scenes); i++) {
        scene_free((Scene *)om_value_at(&anim->scenes, i));
    }
    om_free(&anim->scenes);
    free(anim->input_symbol);
    free(anim->active_scene);
    memset(anim, 0, sizeof(*anim));
}

void animation_set_appearance(Animation *anim, bool uses_input_preexisting_colors, const char *symbol,
                              const ColorPair *colors) {
    doze_wake((CharId)anim->render_id);
    const char *use_symbol = symbol ? symbol : anim->input_symbol;
    ColorPair effective;
    if (colors) {
        effective = *colors;
    } else {
        memset(&effective, 0, sizeof(effective));
    }
    bool bold = false;
    if (anim->existing_color_handling == EXISTING_COLOR_ALWAYS && uses_input_preexisting_colors) {
        effective.has_fg = anim->has_input_fg;
        effective.fg = anim->input_fg_color;
        effective.has_bg = anim->has_input_bg;
        effective.bg = anim->input_bg_color;
        bold = anim->input_bold;
    }

    VisualHandle next = visual_make_appearance(use_symbol, &effective, bold,
                                              anim->no_color, anim->use_xterm_colors);
    if (next == 0 || next == anim->current_visual) {
        return;
    }
    anim->current_visual = next;
    renderer_handle((CharId)anim->render_id, next);
}

const char *animation_new_scene(Animation *anim, bool is_looping, bool has_sync, SyncMetric sync, bool has_ease,
                                Easing ease, const char *scene_id, bool uses_input_preexisting_colors) {
    char auto_id[32];
    if (scene_id == NULL || scene_id[0] == '\0') {
        size_t current_id = om_len(&anim->scenes);
        for (;;) {
            snprintf(auto_id, sizeof(auto_id), "%zu", current_id);
            if (!om_contains(&anim->scenes, auto_id)) {
                break;
            }
            current_id++;
        }
        scene_id = auto_id;
    }
    Scene *scene = scene_new(scene_id, is_looping, has_sync, sync, has_ease, ease, anim->no_color,
                             anim->use_xterm_colors);
    if (!scene) {
        return NULL;
    }
    scene->cold->owner = anim->render_id;
    if (anim->existing_color_handling == EXISTING_COLOR_ALWAYS && uses_input_preexisting_colors) {
        scene->cold->has_preexisting_colors = true;
        scene->cold->preexisting_colors.has_fg = anim->has_input_fg;
        scene->cold->preexisting_colors.fg = anim->input_fg_color;
        scene->cold->preexisting_colors.has_bg = anim->has_input_bg;
        scene->cold->preexisting_colors.bg = anim->input_bg_color;
        scene->cold->preexisting_bold = anim->input_bold;
    }
    om_insert(&anim->scenes, scene_id, scene);
    return om_key_at(&anim->scenes, (size_t)om_slot(&anim->scenes, scene_id));
}

bool animation_active_scene_is_complete(const Animation *anim) {
    if (!anim->active_scene) {
        return true;
    }
    const Scene *scene = anim->active_scene_ref;
    if (!scene) {
        scene = anim->active_scene_slot_valid ? (const Scene *)om_value_at(&anim->scenes, anim->active_scene_slot)
                                              : (const Scene *)om_get(&anim->scenes, anim->active_scene);
    }
    if (!scene) {
        return true;
    }
    return scene->head >= scene->all_frames_len || scene->is_looping;
}

Scene *animation_active_scene(Animation *anim) {
    if (anim->active_scene_ref) {
        return anim->active_scene_ref;
    }
    if (!anim->active_scene) {
        return NULL;
    }
    if (anim->active_scene_slot_valid) {
        return (Scene *)om_value_at(&anim->scenes, anim->active_scene_slot);
    }
    return (Scene *)om_get(&anim->scenes, anim->active_scene);
}

void animation_clear_scenes(Animation *anim) {
    doze_wake((CharId)anim->render_id);
    for (size_t i = 0; i < om_len(&anim->scenes); i++) {
        scene_free((Scene *)om_value_at(&anim->scenes, i));
    }
    om_clear(&anim->scenes);
    anim->active_scene_ref = NULL;
    anim->active_scene_slot_valid = false;
}

// --- frame memo -----------------------------------------------------------

typedef struct {
    char *symbol;  // owned copy for value comparison
    Color fg;
    Scene *scene;  // borrowed: the template scene for this pair
    FrameRun run;  // immutable description captured before the source can change
} FrameMemoEntry;

struct FrameMemo {
    FrameMemoEntry *slots;
    size_t cap;  // power of two
    size_t used;
};

static uint64_t framemo_hash(const char *symbol, Color fg) {
    uint64_t h = str_hash64(symbol);
    h ^= str_hash64(fg.hex) * 0x9E3779B97F4A7C15ULL;
    h ^= (uint64_t)fg.is_xterm << 56;
    h ^= (uint64_t)fg.xterm << 32;
    h ^= (uint64_t)fg.hex_len << 48;
    return h;
}

static bool framemo_key_eq(const FrameMemoEntry *e, const char *symbol, Color fg) {
    return e->fg.is_xterm == fg.is_xterm && e->fg.xterm == fg.xterm && e->fg.hex_len == fg.hex_len &&
           memcmp(e->fg.hex, fg.hex, fg.hex_len) == 0 && strcmp(e->symbol, symbol) == 0;
}

FrameMemo *framemo_new(void) {
    return calloc(1, sizeof(FrameMemo));
}

static FrameMemoEntry *framemo_find(FrameMemo *m, const char *symbol, Color fg) {
    FrameMemo *tab = m;
    if (!tab || tab->cap == 0) {
        return NULL;
    }
    size_t mask = tab->cap - 1;
    size_t i = (size_t)framemo_hash(symbol, fg) & mask;
    while (tab->slots[i].scene) {
        if (framemo_key_eq(&tab->slots[i], symbol, fg)) {
            return &tab->slots[i];
        }
        i = (i + 1) & mask;
    }
    return NULL;
}

Scene *framemo_get(FrameMemo *m, const char *symbol, Color fg) {
    FrameMemoEntry *entry = framemo_find(m, symbol, fg);
    return entry ? entry->scene : NULL;
}

const FrameRun *framemo_get_run(FrameMemo *m, const char *symbol, Color fg) {
    FrameMemoEntry *entry = framemo_find(m, symbol, fg);
    return entry ? &entry->run : NULL;
}

static int framemo_rehash(FrameMemo *tab, size_t cap) {
    FrameMemoEntry *grown = calloc(cap, sizeof(FrameMemoEntry));
    if (!grown) {
        return -1;
    }
    for (size_t i = 0; i < tab->cap; i++) {
        if (!tab->slots[i].scene) {
            continue;
        }
        size_t j = (size_t)framemo_hash(tab->slots[i].symbol, tab->slots[i].fg) & (cap - 1);
        while (grown[j].scene) {
            j = (j + 1) & (cap - 1);
        }
        grown[j] = tab->slots[i];
    }
    free(tab->slots);
    tab->slots = grown;
    tab->cap = cap;
    return 0;
}

int framemo_put(FrameMemo *m, const char *symbol, Color fg, Scene *scene) {
    FrameMemo *tab = m;
    if (!tab) {
        return -1;
    }
    if ((tab->used + 1) * 2 >= tab->cap && framemo_rehash(tab, tab->cap ? tab->cap * 2 : 64) != 0) {
        return -1;
    }
    size_t mask = tab->cap - 1;
    size_t i = (size_t)framemo_hash(symbol, fg) & mask;
    while (tab->slots[i].scene) {
        if (framemo_key_eq(&tab->slots[i], symbol, fg)) {
            return 0;
        }
        i = (i + 1) & mask;
    }
    char *copy = malloc(strlen(symbol) + 1);
    if (!copy) {
        return -1;
    }
    strcpy(copy, symbol);
    tab->slots[i].symbol = copy;
    tab->slots[i].fg = fg;
    tab->slots[i].scene = scene;
    tab->slots[i].run = scene_frame_run(scene);
    tab->used++;
    return 0;
}

void framemo_free(FrameMemo *m) {
    FrameMemo *tab = m;
    if (!tab) {
        return;
    }
    for (size_t i = 0; i < tab->cap; i++) {
        free(tab->slots[i].symbol);
    }
    free(tab->slots);
    free(tab);
}
