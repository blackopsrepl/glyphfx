#include "engine/ctx.h"

#include <stdbool.h>
#include <math.h>
#include <limits.h>

#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
#include <immintrin.h>
#define GLYPHFX_MOTION_AVX2 1
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/canvas.h"
#include "engine/motion.h"
#include "engine/render_log.h"
#include "utils/pycompat.h"
#include "utils/strhash.h"
#include "utils/strtab.h"

int engine_ctx_init(EngineCtx *ctx, const char *input_data, const TerminalConfig *config, Rng rng, Clock clock,
                    PreprocessError *err) {
    memset(ctx, 0, sizeof(*ctx));
    if (terminal_new(&ctx->terminal, input_data, config, err) != 0) {
        return -1;
    }
    ctx->rng = rng;
    ctx->clock = clock;
    ac_init(&ctx->active_characters);
    doze_init(&ctx->doze, ctx);
    ctx->preexisting_colors_present = false;
    for (size_t i = 0; i < ctx->terminal.input_characters_len; i++) {
        EffectCharacter *ch = &ctx->terminal.arena.items[ctx->terminal.input_characters[i]];
        if (ch->animation.has_input_fg || ch->animation.has_input_bg) {
            ctx->preexisting_colors_present = true;
            break;
        }
    }
    return 0;
}

// Flat storage, grown with the arena and owned by this engine context.
static bool mirror_grow(EngineCtx *ctx) {
    size_t slots = ctx->terminal.arena.len;
    if (slots <= ctx->mirrors_cap) {
        return true;
    }
    size_t cap = ctx->mirrors_cap ? ctx->mirrors_cap : 1024;
    while (cap < slots) {
        cap *= 2;
    }
    MotionMirror *grown = realloc(ctx->mirrors, cap * sizeof(MotionMirror));
    if (!grown) {
        return false;
    }
    memset(grown + ctx->mirrors_cap, 0, (cap - ctx->mirrors_cap) * sizeof(MotionMirror));
    ctx->mirrors = grown;
    size_t old_words = ctx->mirrors_cap / 64;
    size_t words = cap / 64;
    MotionWord *input = realloc(ctx->motion_words, words * sizeof(MotionWord));
    if (!input) return false;
    memset(input + old_words, 0, (words - old_words) * sizeof(MotionWord));
    // Inactive lanes may share a SIMD group with active ones. Give their
    // division denominators neutral values, not zero.
    for (size_t w = old_words; w < words; w++) {
        for (unsigned lane = 0; lane < 64; lane++) {
            input[w].max[lane] = 1.0;
            input[w].limit[lane] = 1.0;
        }
    }
    ctx->motion_words = input;
    uint64_t *bits = realloc(ctx->motion_mirrored, words * sizeof(uint64_t));
    if (!bits) return false;
    memset(bits + old_words, 0, (words - old_words) * sizeof(uint64_t));
    ctx->motion_mirrored = bits;
    ctx->mirrors_cap = cap;
    return true;
}

void engine_ctx_free(EngineCtx *ctx) {
    // Drop the dozes first: nothing may settle them into the freed arena.
    doze_free(&ctx->doze);
    free(ctx->mirrors);
    free(ctx->motion_words);
    free(ctx->motion_mirrored);
    while (ctx->motion_eases) {
        MotionEaseTable *next = ctx->motion_eases->next;
        free(ctx->motion_eases);
        ctx->motion_eases = next;
    }
    terminal_free(&ctx->terminal);
    ac_free(&ctx->active_characters);
    memset(ctx, 0, sizeof(*ctx));
}

static bool observes_event(const EngineCtx *ctx, CharId id, Event event) {
    return event_handler_subscribes(&ctx->terminal.arena.items[id].event_handler, event);
}

static CallerKey scene_caller(const Scene *scene) {
    CallerKey key;
    memset(&key, 0, sizeof(key));
    key.kind = CALLER_SCENE;
    key.id = scene->cold->scene_id;
    key.id_handle = strtab_handle(scene->cold->scene_id);
    return key;
}

static CallerKey path_caller(const char *id) {
    CallerKey key;
    memset(&key, 0, sizeof(key));
    key.kind = CALLER_PATH;
    key.id = (char *)id;
    key.id_handle = strtab_handle(id);
    return key;
}

void engine_caller_from_waypoint(const Waypoint *wp, CallerKey *out) {
    memset(out, 0, sizeof(*out));
    out->kind = CALLER_WAYPOINT;
    out->waypoint.coord = wp->coord;
    // The waypoint's name is already interned, so its handle is a lookup by
    // pointer, not a copy; matching then compares the handle and the controls.
    out->waypoint.waypoint_id = (char *)wp->waypoint_id;
    out->waypoint.waypoint_handle = wp->waypoint_id ? strtab_handle(wp->waypoint_id) : 0;
    if (wp->bezier_len) {
        out->waypoint.bezier = malloc(wp->bezier_len * sizeof(Coord));
        memcpy(out->waypoint.bezier, wp->bezier, wp->bezier_len * sizeof(Coord));
    }
    out->waypoint.bezier_len = wp->bezier_len;
}

// --- events ---------------------------------------------------------------

int engine_register_event(EngineCtx *ctx, CharId id, Event event, const CallerKey *caller, const EventAction *action) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    if (caller->kind == CALLER_PATH && !om_contains(&ch->motion.paths, caller->id)) {
        return -1;
    }
    if (caller->kind == CALLER_SCENE && !om_contains(&ch->animation.scenes, caller->id)) {
        return -1;
    }
    if ((action->kind == ACTION_ACTIVATE_PATH || action->kind == ACTION_DEACTIVATE_PATH) && action->has_id &&
        !om_contains(&ch->motion.paths, action->id)) {
        return -1;
    }
    if ((action->kind == ACTION_ACTIVATE_SCENE || action->kind == ACTION_DEACTIVATE_SCENE) && action->has_id &&
        !om_contains(&ch->animation.scenes, action->id)) {
        return -1;
    }
    return event_handler_push(&ch->event_handler, event, caller, action);
}

int engine_chain_paths(EngineCtx *ctx, CharId id, const char *const *paths, size_t n_paths, bool loop) {
    if (n_paths < 2) {
        return 0;
    }
    for (size_t i = 1; i < n_paths; i++) {
        EventAction action;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_PATH;
        action.id = (char *)paths[i];
        CallerKey caller = path_caller(paths[i - 1]);
        if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
            return -1;
        }
    }
    if (loop) {
        EventAction action;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_PATH;
        action.id = (char *)paths[0];
        CallerKey caller = path_caller(paths[n_paths - 1]);
        if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
            return -1;
        }
    }
    return 0;
}

void engine_handle_event(EngineCtx *ctx, Effect *effect, CharId id, Event event, const CallerKey *caller) {
    // The matching entry index is stable for the whole dispatch: actions only
    // ever append (to this entry or a new one at the end), so resolve it once
    // instead of re-hashing the caller once per action.
    EventHandler *first = &ctx->terminal.arena.items[id].event_handler;
    long entry = event_handler_actions_index(first, event, caller);
    if (entry < 0) {
        return;
    }
    // Only an actual dispatch can change another slot. Its pending batch
    // result expires, but unrelated persistent mirrors remain reusable.
    ctx->motion_epoch++;
    for (size_t action_index = 0;; action_index++) {
        EventHandler *handler = &ctx->terminal.arena.items[id].event_handler;
        if ((size_t)entry >= handler->len) {
            return;
        }
        size_t count = event_handler_action_count(handler, (size_t)entry);
        if (action_index >= count) {
            return;
        }
        const EventAction *action = event_handler_action(handler, (size_t)entry, action_index);
        // A dispatched action can append to the entry and reallocate its action
        // list, so the action cannot be read across the dispatch. Non-callbacks
        // only need the id to survive; keep it on the stack instead of a heap
        // deep copy. Callbacks own argument strings, so those are deep-copied.
        char id_buf[256];
        EventAction local;
        bool deep = false;
        size_t id_len = action->id ? strlen(action->id) : 0;
        if (action->kind == ACTION_CALLBACK || id_len >= sizeof(id_buf)) {
            event_action_copy(&local, action);
            deep = true;
        } else {
            local = *action;
            if (action->id) {
                memcpy(id_buf, action->id, id_len + 1);
                local.id = id_buf;
            }
        }
        switch (local.kind) {
            case ACTION_ACTIVATE_SCENE:
                engine_activate_scene(ctx, effect, id, local.id);
                break;
            case ACTION_DEACTIVATE_SCENE:
                engine_deactivate_scene(ctx, id, local.has_id ? local.id : NULL);
                break;
            case ACTION_ACTIVATE_PATH:
                engine_activate_path(ctx, effect, id, local.id);
                break;
            case ACTION_DEACTIVATE_PATH:
                motion_deactivate_path(&ctx->terminal.arena.items[id].motion, local.has_id ? local.id : NULL);
                break;
            case ACTION_RESET_APPEARANCE: {
                EffectCharacter *ch = &ctx->terminal.arena.items[id];
                animation_set_appearance(&ch->animation, ch->uses_input_preexisting_colors, ch->input_symbol, NULL);
                break;
            }
            case ACTION_SET_LAYER:
                ctx->terminal.arena.items[id].layer = local.layer;
                renderer_layer(id, local.layer);
                break;
            case ACTION_SET_COORDINATE:
                motion_set_coordinate(&ctx->terminal.arena.items[id].motion, local.coord);
                break;
            case ACTION_CALLBACK:
                if (effect->ops->dispatch_callback) {
                    ac_mark_all_candidates(&ctx->active_characters);
                    effect->ops->dispatch_callback(effect, ctx, id, &local.cb);
                }
                break;
        }
        if (deep) {
            event_action_free(&local);
        }
    }
}

// --- motion ---------------------------------------------------------------

static void path_replace_segment(Path *p, size_t index, const Segment *seg) {
    p->geometry_revision++;
    segment_free(&p->segments[index]);
    segment_copy(&p->segments[index], seg);
}

static void path_insert_segment_front(Path *p, const Segment *seg) {
    p->geometry_revision++;
    if (p->segments_len == p->segments_cap) {
        size_t cap = p->segments_cap ? p->segments_cap * 2 : 8;
        Segment *grown = realloc(p->segments, cap * sizeof(Segment));
        if (!grown) {
            return;
        }
        p->segments = grown;
        p->segments_cap = cap;
    }
    memmove(p->segments + 1, p->segments, p->segments_len * sizeof(Segment));
    p->segments_len++;
    memset(&p->segments[0], 0, sizeof(Segment));
    segment_copy(&p->segments[0], seg);
}

void engine_activate_path(EngineCtx *ctx, Effect *effect, CharId id, const char *path_id) {
    doze_wake(id);
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    long slot = om_slot(&ch->motion.paths, path_id);
    Path *p = slot < 0 ? NULL : (Path *)om_value_at(&ch->motion.paths, (size_t)slot);
    if (!p || p->waypoints_len == 0) {
        return;
    }
    Coord current = ch->motion.current_coord;
    const Waypoint *first = &p->waypoints[0];
    double distance_to_first = first->has_bezier
                                   ? find_length_of_bezier_curve(current, first->bezier, first->bezier_len,
                                                                 first->coord)
                                   : find_length_of_line(current, first->coord, true);

    Segment new_origin;
    memset(&new_origin, 0, sizeof(new_origin));
    new_origin.start.waypoint_id = strtab_intern("origin");
    new_origin.start.coord = current;
    waypoint_copy(&new_origin.end, first);
    new_origin.distance = distance_to_first;

    free(ch->motion.active_path);
    ch->motion.active_path = malloc(strlen(path_id) + 1);
    strcpy(ch->motion.active_path, path_id);
    ch->motion.active_path_slot = (size_t)slot;
    ch->motion.active_path_slot_valid = true;

    p->total_distance += distance_to_first;
    if (p->has_origin_segment) {
        p->total_distance -= p->origin_segment.distance;
        path_replace_segment(p, 0, &new_origin);
        // release the old origin's stored copy
        if (p->has_origin_segment) {
            segment_free(&p->origin_segment);
        }
    } else {
        path_insert_segment_front(p, &new_origin);
    }
    memset(&p->origin_segment, 0, sizeof(p->origin_segment));
    segment_copy(&p->origin_segment, &new_origin);
    p->has_origin_segment = true;
    p->current_step = 0;
    if ((size_t)id < ctx->mirrors_cap) {
        MotionMirror *mm = &ctx->mirrors[id];
        ctx->motion_mirrored[(size_t)id >> 6] &= ~(UINT64_C(1) << ((unsigned)id & 63u));
        mm->valid = false;
        mm->path = p;
        mm->step = 0;
    }
    p->hold_time_remaining = p->hold_time;
    p->max_steps = py_round_half_even(p->total_distance / p->speed);
    for (size_t i = 0; i < p->segments_len; i++) {
        p->segments[i].enter_event_triggered = false;
        p->segments[i].exit_event_triggered = false;
    }
    if (p->has_layer) {
        ch->layer = p->layer;
        renderer_layer(id, p->layer);
    }
    segment_free(&new_origin);
    if (observes_event(ctx, id, EVENT_PATH_ACTIVATED)) {
        CallerKey caller = path_caller(path_id);
        engine_handle_event(ctx, effect, id, EVENT_PATH_ACTIVATED, &caller);
    }
}

static bool motion_ease_equal(const Easing *a, const Easing *b) {
    if (a->kind != b->kind) return false;
    return a->kind != EASE_CUBIC_BEZIER ||
           (a->x1 == b->x1 && a->y1 == b->y1 && a->x2 == b->x2 && a->y2 == b->y2);
}

// fx Paths::ease_table: paths sharing an easing/step count share factors.
// Tables are optional and bounded; huge or numerous shapes keep scalar ease
// evaluation rather than growing memory without limit.
static const double *motion_ease_table(EngineCtx *ctx, const Path *p) {
    if (!p->has_ease || p->max_steps <= 0 || p->max_steps > (1 << 16)) return NULL;
    for (MotionEaseTable *t = ctx->motion_eases; t; t = t->next) {
        if (t->steps == p->max_steps && motion_ease_equal(&t->ease, &p->ease))
            return t->factors;
    }
    size_t count = (size_t)p->max_steps + 1;
    if (ctx->motion_ease_values + count > ((size_t)1 << 20)) return NULL;
    MotionEaseTable *t = malloc(sizeof(*t) + count * sizeof(double));
    if (!t) return NULL;
    t->ease = p->ease;
    t->steps = p->max_steps;
    for (size_t i = 0; i < count; i++)
        t->factors[i] = easing_ease(&p->ease, (double)i / (double)p->max_steps);
    t->next = ctx->motion_eases;
    ctx->motion_eases = t;
    ctx->motion_ease_values += count;
    return t->factors;
}

// fx motion.rs::mirror: only combine preceding subtractions when exact.
// Summing fractional prefixes changes rounding and was the old cursor bug.
static void mirror_capture(EngineCtx *ctx, CharId id, Path *p, size_t k, bool over) {
    if ((size_t)id >= ctx->mirrors_cap) return;
    MotionMirror *m = &ctx->mirrors[id];
    ctx->motion_mirrored[(size_t)id >> 6] &= ~(UINT64_C(1) << ((unsigned)id & 63u));
    m->valid = false;
    if (k >= p->segments_len || p->max_steps <= 0 || p->max_steps > INT64_C(4503599627370496) ||
        !(fabs(p->total_distance) < 0x1p52) || p->total_distance == 0.0) return;
    Segment *s = &p->segments[k];
    if (s->end.has_bezier && s->end.bezier_len > 1) return;
    size_t passed = k + (over ? 1u : 0u);
    double off = 0.0;
    for (size_t j = 0; j < passed; j++) {
        double d = p->segments[j].distance;
        if (passed > 1 && trunc(d) != d) return;
        off += d;
    }
    if (!(off < 0x1p52)) return;
    m->path = p;
    m->segment = k;
    m->over = over;
    m->segments_len = p->segments_len;
    m->identity = p->identity;
    m->geometry = p->geometry_revision;
    m->step = p->current_step;
    m->max_steps = p->max_steps;
    m->total = p->total_distance;
    m->offset = off;
    m->limit = s->distance;
    m->start = s->start.coord;
    m->end = s->end.coord;
    m->curved = s->end.has_bezier && s->end.bezier_len == 1;
    m->control = m->curved ? s->end.bezier[0] : m->end;
    m->has_ease = p->has_ease;
    m->ease = p->ease;
    m->factors = motion_ease_table(ctx, p);
    MotionWord *word = &ctx->motion_words[(size_t)id >> 6];
    unsigned lane = (unsigned)id & 63u;
    word->step[lane] = (double)m->step;
    word->max[lane] = (double)m->max_steps;
    word->total[lane] = m->total;
    word->offset[lane] = m->offset;
    word->limit[lane] = m->limit;
    word->sx[lane] = (double)m->start.column;
    word->sy[lane] = (double)m->start.row;
    word->cx[lane] = (double)m->control.column;
    word->cy[lane] = (double)m->control.row;
    word->ex[lane] = (double)m->end.column;
    word->ey[lane] = (double)m->end.row;
    m->valid = true;
    ctx->motion_mirrored[(size_t)id >> 6] |= UINT64_C(1) << lane;
}

// fx's pure-segment arithmetic, isolated from record mutation. This is also
// the lane kernel for the word pass: divide, ease, multiply, subtract, divide,
// interpolate, half-even round. No reassociation or reciprocal substitution.
static bool mirror_next(const MotionMirror *m, Coord *out, double *distance) {
    if (!m->valid || m->step >= m->max_steps) return false;
    double ratio = (double)(m->step + 1) / (double)m->max_steps;
    double factor = !m->has_ease ? ratio :
                    m->factors ? m->factors[m->step + 1] : easing_ease(&m->ease, ratio);
    double d = factor * m->total;
    double remaining = d - m->offset;
    if (m->over) {
        if (!(d > m->offset)) return false;
        remaining += m->limit;
    } else if (!(remaining <= m->limit && (m->segment == 0 || d > m->offset))) {
        return false;
    }
    double t = m->limit == 0.0 ? 0.0 : remaining / m->limit;
    if (!m->has_ease && !(t < 1.0)) t = 1.0;
    *out = m->curved ? find_coord_on_bezier_curve(m->start, &m->control, 1, m->end, t)
                     : find_coord_on_line(m->start, m->end, t);
    *distance = d;
    return true;
}

#ifdef GLYPHFX_MOTION_AVX2
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((target("avx2,no-fma"), optimize("fp-contract=off")))
#else
__attribute__((target("avx2,no-fma")))
#endif
static void motion_four(EngineCtx *ctx, size_t word, const unsigned lanes[4]) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
    const MotionMirror *a = &ctx->mirrors[(word << 6) | lanes[0]];
    const MotionMirror *b = &ctx->mirrors[(word << 6) | lanes[1]];
    const MotionMirror *c = &ctx->mirrors[(word << 6) | lanes[2]];
    const MotionMirror *d = &ctx->mirrors[(word << 6) | lanes[3]];
    const MotionWord *input = &ctx->motion_words[word];
    __m128i indices = _mm_set_epi32((int)lanes[3], (int)lanes[2], (int)lanes[1], (int)lanes[0]);
    bool adjacent = lanes[1] == lanes[0] + 1 && lanes[2] == lanes[0] + 2 &&
                    lanes[3] == lanes[0] + 3;
#define PACK(field) (adjacent ? _mm256_loadu_pd(input->field + lanes[0]) \
                              : _mm256_i32gather_pd(input->field, indices, 8))
    const __m256d one = _mm256_set1_pd(1.0);
    __m256d step = PACK(step);
    step = _mm256_add_pd(step, one);
    __m256d factor = (a->has_ease && b->has_ease && c->has_ease && d->has_ease)
                         ? _mm256_setzero_pd() : _mm256_div_pd(step, PACK(max));
    __m256d eased = _mm256_set_pd(
        d->factors ? d->factors[d->step + 1] : 0.0,
        c->factors ? c->factors[c->step + 1] : 0.0,
        b->factors ? b->factors[b->step + 1] : 0.0,
        a->factors ? a->factors[a->step + 1] : 0.0);
    __m256d ease_mask = _mm256_castsi256_pd(_mm256_set_epi64x(
        d->has_ease ? -1LL : 0LL, c->has_ease ? -1LL : 0LL,
        b->has_ease ? -1LL : 0LL, a->has_ease ? -1LL : 0LL));
    factor = _mm256_blendv_pd(factor, eased, ease_mask);
    __m256d travel = _mm256_mul_pd(factor, PACK(total));
    __m256d remaining = _mm256_sub_pd(travel, PACK(offset));
    __m256d limit = PACK(limit);
    __m256d over_mask = _mm256_castsi256_pd(_mm256_set_epi64x(
        d->over ? -1LL : 0LL, c->over ? -1LL : 0LL,
        b->over ? -1LL : 0LL, a->over ? -1LL : 0LL));
    remaining = _mm256_blendv_pd(remaining, _mm256_add_pd(remaining, limit), over_mask);
    __m256d fraction = _mm256_div_pd(remaining, limit);
    // The scalar path uses t=0 for a zero-length segment, never 0/0.
    fraction = _mm256_blendv_pd(fraction, _mm256_setzero_pd(),
                               _mm256_cmp_pd(limit, _mm256_setzero_pd(), _CMP_EQ_OQ));
    fraction = _mm256_blendv_pd(_mm256_min_pd(fraction, one), fraction, ease_mask);
    __m256d u = _mm256_sub_pd(one, fraction);
    __m256d x = _mm256_add_pd(_mm256_mul_pd(u, PACK(sx)),
                             _mm256_mul_pd(fraction, PACK(ex)));
    __m256d y = _mm256_add_pd(_mm256_mul_pd(u, PACK(sy)),
                             _mm256_mul_pd(fraction, PACK(ey)));
    // Straight-only groups do not pay for the quadratic kernel.
    if (a->curved || b->curved || c->curved || d->curved) {
    // Quadratic lanes use the same two de Casteljau stages as geometry.c.
    __m256d ax = _mm256_add_pd(_mm256_mul_pd(u, PACK(sx)),
                              _mm256_mul_pd(fraction, PACK(cx)));
    __m256d ay = _mm256_add_pd(_mm256_mul_pd(u, PACK(sy)),
                              _mm256_mul_pd(fraction, PACK(cy)));
    __m256d bx = _mm256_add_pd(_mm256_mul_pd(u, PACK(cx)),
                              _mm256_mul_pd(fraction, PACK(ex)));
    __m256d by = _mm256_add_pd(_mm256_mul_pd(u, PACK(cy)),
                              _mm256_mul_pd(fraction, PACK(ey)));
    __m256d curve_mask = _mm256_castsi256_pd(_mm256_set_epi64x(
        d->curved ? -1LL : 0LL, c->curved ? -1LL : 0LL,
        b->curved ? -1LL : 0LL, a->curved ? -1LL : 0LL));
    x = _mm256_blendv_pd(x, _mm256_add_pd(_mm256_mul_pd(u, ax),
                                       _mm256_mul_pd(fraction, bx)), curve_mask);
    y = _mm256_blendv_pd(y, _mm256_add_pd(_mm256_mul_pd(u, ay),
                                       _mm256_mul_pd(fraction, by)), curve_mask);
    }
    // Round explicitly, independently of the caller's MXCSR rounding mode.
    x = _mm256_round_pd(x, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    y = _mm256_round_pd(y, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m256d above_offset = _mm256_cmp_pd(travel, PACK(offset), _CMP_GT_OQ);
    __m256d lower = _mm256_or_pd(_mm256_castsi256_pd(_mm256_set_epi64x(d->segment == 0 ? -1LL : 0LL,
                                                      c->segment == 0 ? -1LL : 0LL,
                                                      b->segment == 0 ? -1LL : 0LL,
                                                      a->segment == 0 ? -1LL : 0LL)),
                                 above_offset);
    __m256d in_segment = _mm256_and_pd(_mm256_cmp_pd(remaining, limit, _CMP_LE_OQ), lower);
    in_segment = _mm256_blendv_pd(in_segment, above_offset, over_mask);
    __m256d ok = _mm256_and_pd(_mm256_cmp_pd(step, PACK(max), _CMP_LE_OQ), in_segment);
    __m256d low = _mm256_set1_pd((double)INT32_MIN);
    __m256d high = _mm256_set1_pd((double)INT32_MAX);
    ok = _mm256_and_pd(ok, _mm256_and_pd(_mm256_cmp_pd(x, low, _CMP_GE_OQ),
                                       _mm256_cmp_pd(x, high, _CMP_LE_OQ)));
    ok = _mm256_and_pd(ok, _mm256_and_pd(_mm256_cmp_pd(y, low, _CMP_GE_OQ),
                                       _mm256_cmp_pd(y, high, _CMP_LE_OQ)));
    unsigned mask = (unsigned)_mm256_movemask_pd(ok);
    int32_t xs[4], ys[4];
    double distances[4];
    _mm_storeu_si128((__m128i *)(void *)xs, _mm256_cvttpd_epi32(x));
    _mm_storeu_si128((__m128i *)(void *)ys, _mm256_cvttpd_epi32(y));
    _mm256_storeu_pd(distances, travel);
    const MotionMirror *mirrors[4] = {a, b, c, d};
    for (unsigned i = 0; i < 4; i++) {
        unsigned lane = lanes[i];
        if (mask & (1u << i)) {
            ctx->motion_batch.coord[lane] = coord_new(xs[i], ys[i]);
            ctx->motion_batch.distance[lane] = distances[i];
            ctx->motion_batch.step[lane] = mirrors[i]->step;
            ctx->motion_batch.ready |= UINT64_C(1) << lane;
        } else if (mirror_next(mirrors[i], &ctx->motion_batch.coord[lane],
                              &ctx->motion_batch.distance[lane])) {
            // Large coordinates keep the original int64 rounding semantics.
            ctx->motion_batch.step[lane] = mirrors[i]->step;
            ctx->motion_batch.ready |= UINT64_C(1) << lane;
        }
    }
#undef PACK
}
#endif

// A path lane is idle only if its next coordinate AND animation are unchanged.
// Synced scenes do not advance counters: evaluate the original sync formula
// against the speculative step/distance and compare the resulting visual.
static bool motion_synced_visual(const EffectCharacter *ch, const MotionMirror *m,
                                 double distance, VisualHandle *visual) {
    *visual = 0;
    const Animation *a = &ch->animation;
    if (!a->active_scene) return true;
    const Scene *s = a->active_scene_ref;
    if (!s) return false;  // legacy lookup must run through the normal tick
    if (s->head >= s->all_frames_len) return true;
    if (!s->has_sync || s->is_looping) return false;
    int64_t last = (int64_t)(s->all_frames_len - s->head) - 1;
    double ratio;
    if (s->sync == SYNC_STEP) {
        int64_t step = m->step + 1;
        int64_t max = m->max_steps;
        ratio = (double)(step > 1 ? step : 1) / (double)(max > 1 ? max : 1);
    } else {
        double total = m->total > 1.0 ? m->total : 1.0;
        double remaining = m->total - distance;
        if (remaining < 1.0) remaining = 1.0;
        double reached = total - remaining;
        if (reached < 1.0) reached = 1.0;
        ratio = reached / total;
    }
    int64_t index = py_round_half_even((double)last * ratio);
    if (index > last) index = last;
    if (index < 0) index = 0;
    *visual = s->all_frames[s->head + (size_t)index].visual;
    return true;
}

static unsigned motion_scene_flags(const Scene *scene) {
    return (scene->has_sync ? 1u : 0u) | (scene->is_looping ? 2u : 0u) |
           ((unsigned)scene->sync << 2);
}

// Between two of a slot's own commits, the mirror's own fields are the truth
// for its pure steps: every mutation point outside them either bumps the
// epoch (any dispatched callback), clears this slot's mirror bit (path
// activation, ring disperse's map swap) or bumps the path's geometry revision
// (waypoint changes). Validating the segment geometry per preparation instead
// cost binarypath a quarter of its run for no information. fx trusts its
// mirror the same way; the fallback walk re-reads the path when anything did
// move.
static bool mirror_live(MotionMirror *m) {
    return m->valid && m->step < m->max_steps && m->path != NULL &&
           m->path->identity == m->identity &&
           m->path->geometry_revision == m->geometry;
}

// Compute a whole snapshot word before its ordered ticks. Results are only
// committed when that slot's turn arrives. Thus a callback can
// invalidate later results without exposing any future path state to effects.
static void motion_prepare_word(EngineCtx *ctx, size_t word, uint64_t slots) {
    MotionBatch *batch = &ctx->motion_batch;
    batch->ready = 0;
    batch->idle = 0;
    batch->animation = 0;
    batch->word = word;
    batch->epoch = ctx->motion_epoch;
#ifdef GLYPHFX_MOTION_AVX2
    unsigned lanes[4];
    unsigned count = 0;
    bool vector = __builtin_cpu_supports("avx2") != 0;
#endif
    while (slots) {
        unsigned lane = (unsigned)__builtin_ctzll(slots);
        slots &= slots - 1;
        size_t id = (word << 6) | lane;
        if (id >= ctx->mirrors_cap) continue;
        MotionMirror *m = &ctx->mirrors[id];
        if (!mirror_live(m)) {
            m->valid = false;
            ctx->motion_mirrored[word] &= ~(UINT64_C(1) << lane);
            continue;
        }
#ifdef GLYPHFX_MOTION_AVX2
        if (vector && (!m->has_ease || m->factors) && m->step < m->max_steps) {
            lanes[count++] = lane;
            if (count == 4) {
                motion_four(ctx, word, lanes);
                count = 0;
            }
            continue;
        }
#endif
        if (mirror_next(m, &batch->coord[lane], &batch->distance[lane])) {
            batch->step[lane] = m->step;
            batch->ready |= UINT64_C(1) << lane;
        }
    }
#ifdef GLYPHFX_MOTION_AVX2
    for (unsigned i = 0; i < count; i++) {
        unsigned lane = lanes[i];
        const MotionMirror *m = &ctx->mirrors[(word << 6) | lane];
        if (mirror_next(m, &batch->coord[lane], &batch->distance[lane])) {
            batch->step[lane] = m->step;
            batch->ready |= UINT64_C(1) << lane;
        }
    }
#endif
    uint64_t ready = batch->ready;
    while (ready) {
        unsigned lane = (unsigned)__builtin_ctzll(ready);
        ready &= ready - 1;
        size_t id = (word << 6) | lane;
        const MotionMirror *m = &ctx->mirrors[id];
        const EffectCharacter *ch = &ctx->terminal.arena.items[id];
        if (motion_synced_visual(ch, m, batch->distance[lane], &batch->visual[lane])) {
            const Animation *a = &ch->animation;
            Scene *scene = a->active_scene ? a->active_scene_ref : NULL;
            batch->scene[lane] = scene;
            batch->scene_head[lane] = scene ? scene->head : 0;
            batch->scene_count[lane] = scene ? scene->all_frames_len : 0;
            batch->scene_flags[lane] = scene ? motion_scene_flags(scene) : 0;
            batch->animation |= UINT64_C(1) << lane;
            // Final steps must still execute the holding/completion tail.
            if (m->step + 1 < m->max_steps &&
                coord_eq(batch->coord[lane], ch->motion.current_coord) &&
                (batch->visual[lane] == 0 ||
                 batch->visual[lane] == a->current_visual)) {
                batch->idle |= UINT64_C(1) << lane;
            }
        }
    }
}

// Pure steps stay inside the mirrored segment. Boundary crossings take the
// original walk, which owns segment events and refreshes the mirror afterwards.
static bool mirror_step(EngineCtx *ctx, CharId id, Path *p, Coord *out) {
    if ((size_t)id >= ctx->mirrors_cap) return false;
    MotionMirror *m = &ctx->mirrors[id];
    if (!mirror_live(m) || m->path != p) return false;
    MotionBatch *batch = &ctx->motion_batch;
    unsigned lane = (unsigned)id & 63u;
    uint64_t bit = UINT64_C(1) << lane;
    double d;
    if (batch->word == (size_t)id >> 6 && batch->epoch == ctx->motion_epoch &&
        (batch->ready & bit) && batch->step[lane] == p->current_step) {
        *out = batch->coord[lane];
        d = batch->distance[lane];
        batch->ready &= ~bit;
    } else if (!mirror_next(m, out, &d)) {
        return false;
    }
    p->current_step++;
    p->last_distance_reached = d;
    m->step = p->current_step;
    ctx->motion_words[(size_t)id >> 6].step[lane] = (double)m->step;
    return true;
}

// Apply a prepared lane directly, without motion_move_now -> path_step and
// their repeated map resolution. Intermediate pure steps cannot hold, loop or
// complete a path. Final steps and segment boundaries use the original walk.
// No Path was changed during preparation: callbacks always see canonical order.
__attribute__((always_inline))
static inline bool motion_take_batch(EngineCtx *ctx, CharId id, bool *animation_ready,
                              VisualHandle *visual) {
    MotionBatch *batch = &ctx->motion_batch;
    unsigned lane = (unsigned)id & 63u;
    uint64_t bit = UINT64_C(1) << lane;
    if (batch->word != ((size_t)id >> 6) || batch->epoch != ctx->motion_epoch ||
        !(batch->ready & bit)) return false;
    MotionMirror *m = &ctx->mirrors[id];
    if (!(ctx->motion_mirrored[batch->word] & bit) || !m->valid ||
        m->step != batch->step[lane] || m->step >= m->max_steps - 1) return false;
    // This mirror passed preparation and nothing outside this slot's own
    // commit could have moved it since: a dispatched callback expires the
    // whole batch (epoch), a path activation or a retired lane clears the
    // mirror bit. The prepared pointer is the path; no map traversal again.
    Path *p = m->path;
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    // Synced animation consumes the same speculative progress as motion; it
    // need not resolve this path again and repeat the progress computation.
    // Non-synced and looping scenes retain their original event/counter path.
    const Animation *a = &ch->animation;
    Scene *scene = a->active_scene ? a->active_scene_ref : NULL;
    bool same_scene = (!a->active_scene || scene != NULL) && scene == batch->scene[lane];
    if ((batch->animation & bit) && same_scene &&
        (!scene || (scene->head == batch->scene_head[lane] &&
                    scene->all_frames_len == batch->scene_count[lane] &&
                    motion_scene_flags(scene) == batch->scene_flags[lane]))) {
        *animation_ready = true;
        // An idle lane can retain its current visual without any frame read.
        *visual = (batch->idle & bit) &&
                  (batch->visual[lane] == 0 || batch->visual[lane] == a->current_visual)
                      ? a->current_visual : batch->visual[lane];
    } else {
        *animation_ready = motion_synced_visual(ch, m, batch->distance[lane], visual);
    }
    Coord next = batch->coord[lane];
    batch->ready &= ~bit;
    p->current_step++;
    p->last_distance_reached = batch->distance[lane];
    m->step = p->current_step;
    ctx->motion_words[batch->word].step[lane] = (double)m->step;
    ch->motion.previous_coord = ch->motion.current_coord;
    // Preserve the renderer's replay contract, including unchanged coordinates
    // after another move within this frame.
    motion_set_coordinate(&ch->motion, next);
    return true;
}

// Retire counter-only lanes before the next observable tick, not during word
// preparation. Callbacks therefore see earlier slots advanced and later slots
// untouched, without speculative writes or a rollback pass. Failed validations
// rejoin the scalar word; only intermediate, unchanged-coordinate steps qualify.
static uint64_t motion_retire_idle(EngineCtx *ctx, uint64_t slots) {
    MotionBatch *batch = &ctx->motion_batch;
    MotionWord *input = &ctx->motion_words[batch->word];
    uint64_t retry = 0;
    while (slots) {
        unsigned lane = (unsigned)__builtin_ctzll(slots);
        uint64_t bit = UINT64_C(1) << lane;
        slots &= slots - 1;
        size_t id = (batch->word << 6) | lane;
        MotionMirror *m = &ctx->mirrors[id];
        if (!(ctx->motion_mirrored[batch->word] & bit) || !mirror_live(m) ||
            m->step != batch->step[lane] || m->path->current_step != m->step) {
            retry = bit | slots;
            break;
        }
        Path *p = m->path;
        p->current_step++;
        p->last_distance_reached = batch->distance[lane];
        m->step = p->current_step;
        input->step[lane] = (double)m->step;
        Motion *motion = &ctx->terminal.arena.items[id].motion;
        motion->previous_coord = motion->current_coord;
        batch->ready &= ~bit;
    }
    return retry;
}

static Coord path_step(EngineCtx *ctx, Effect *effect, CharId id, const char *path_id) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    Motion *m = &ch->motion;
    Path *p = m->active_path_slot_valid ? (Path *)om_value_at(&m->paths, m->active_path_slot) : NULL;
    if (!p) {
        long s = om_slot(&m->paths, path_id);
        if (s < 0) {
            return m->current_coord;
        }
        m->active_path_slot = (size_t)s;
        m->active_path_slot_valid = true;
        p = (Path *)om_value_at(&m->paths, (size_t)s);
    }
    if (!p) {
        return m->current_coord;
    }
    Coord mirrored;
    if (mirror_step(ctx, id, p, &mirrored)) return mirrored;
    if (p->max_steps == 0 || p->current_step >= p->max_steps || p->total_distance == 0.0) {
        return p->segments[p->segments_len - 1].end.coord;
    }
    p->current_step += 1;
    double ratio = (double)p->current_step / (double)p->max_steps;
    double distance_factor = p->has_ease ? easing_ease(&p->ease, ratio) : ratio;
    double distance_to_travel = distance_factor * p->total_distance;
    p->last_distance_reached = distance_to_travel;

    long active = -1;
    size_t i = 0;
    // A dispatched event can mutate the path map (the handler runs reentrantly),
    // so re-resolve after any dispatch; otherwise the pointer above is kept.
    bool dispatched = false;
    for (;;) {
        if (i >= p->segments_len) {
            break;
        }
        double seg_distance = p->segments[i].distance;
        bool enter_triggered = p->segments[i].enter_event_triggered;
        bool exit_triggered = p->segments[i].exit_event_triggered;
        if (distance_to_travel <= seg_distance) {
            active = (long)i;
            if (!enter_triggered) {
                p->segments[i].enter_event_triggered = true;
                if (observes_event(ctx, id, EVENT_SEGMENT_ENTERED)) {
                    CallerKey key;
                    engine_caller_from_waypoint(&p->segments[i].end, &key);
                    engine_handle_event(ctx, effect, id, EVENT_SEGMENT_ENTERED, &key);
                    caller_key_free(&key);
                    dispatched = true;
                }
            }
            break;
        }
        distance_to_travel -= seg_distance;
        if (!enter_triggered || !exit_triggered) {
            bool observes = observes_event(ctx, id, EVENT_SEGMENT_ENTERED) ||
                            observes_event(ctx, id, EVENT_SEGMENT_EXITED);
            if (!observes) {
                p->segments[i].enter_event_triggered = true;
                p->segments[i].exit_event_triggered = true;
            } else {
                if (!enter_triggered) {
                    p->segments[i].enter_event_triggered = true;
                    CallerKey key;
                    engine_caller_from_waypoint(&p->segments[i].end, &key);
                    engine_handle_event(ctx, effect, id, EVENT_SEGMENT_ENTERED, &key);
                    caller_key_free(&key);
                    dispatched = true;
                }
                p = (Path *)om_value_at(&m->paths, m->active_path_slot);
                if (!p) {
                    return m->current_coord;
                }
                if (!p->segments[i].exit_event_triggered) {
                    p->segments[i].exit_event_triggered = true;
                    CallerKey key;
                    engine_caller_from_waypoint(&p->segments[i].end, &key);
                    engine_handle_event(ctx, effect, id, EVENT_SEGMENT_EXITED, &key);
                    caller_key_free(&key);
                    dispatched = true;
                }
            }
        }
        i += 1;
    }
    if (dispatched) {
        p = m->active_path_slot_valid ? (Path *)om_value_at(&m->paths, m->active_path_slot) : NULL;
    }
    if (!p || p->segments_len == 0) {
        return m->current_coord;
    }
    bool over = active < 0;
    if (over) {
        active = (long)p->segments_len - 1;
        distance_to_travel += p->segments[active].distance;
    }
    if (!dispatched) mirror_capture(ctx, id, p, (size_t)active, over);
    Segment *seg = &p->segments[active];
    double t;
    if (seg->distance == 0.0) {
        t = 0.0;
    } else if (p->has_ease) {
        t = distance_to_travel / seg->distance;
    } else {
        double v = distance_to_travel / seg->distance;
        t = v < 1.0 ? v : 1.0;
    }
    if (seg->end.has_bezier) {
        return find_coord_on_bezier_curve(seg->start.coord, seg->end.bezier, seg->end.bezier_len, seg->end.coord, t);
    }
    return find_coord_on_line(seg->start.coord, seg->end.coord, t);
}

__attribute__((always_inline))
static inline void motion_move_now(EngineCtx *ctx, Effect *effect, CharId id) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    ch->motion.previous_coord = ch->motion.current_coord;
    if (!ch->motion.active_path) {
        return;
    }
    const char *active = ch->motion.active_path;
    Path *p = ch->motion.active_path_slot_valid
                  ? (Path *)om_value_at(&ch->motion.paths, ch->motion.active_path_slot)
                  : (Path *)om_get(&ch->motion.paths, active);
    if (!p || p->segments_len == 0) {
        return;
    }
    Coord new_coord = path_step(ctx, effect, id, active);
    ch = &ctx->terminal.arena.items[id];
    motion_set_coordinate(&ch->motion, new_coord);

    active = ch->motion.active_path;
    if (!active) {
        return;
    }
    p = ch->motion.active_path_slot_valid
            ? (Path *)om_value_at(&ch->motion.paths, ch->motion.active_path_slot)
            : (Path *)om_get(&ch->motion.paths, active);
    if (!p) {
        return;
    }
    if (p->current_step == p->max_steps) {
        if (p->hold_time != 0 && p->hold_time_remaining == p->hold_time) {
            if (observes_event(ctx, id, EVENT_PATH_HOLDING)) {
                CallerKey caller = path_caller(active);
                engine_handle_event(ctx, effect, id, EVENT_PATH_HOLDING, &caller);
            }
            p->hold_time_remaining -= 1;
            return;
        }
        if (p->hold_time_remaining != 0) {
            p->hold_time_remaining -= 1;
            return;
        }
        if (p->loop_ && p->segments_len > 1) {
            // Copy the id before deactivating: deactivate frees active_path.
            char *loop_id = malloc(strlen(active) + 1);
            strcpy(loop_id, active);
            motion_deactivate_path(&ch->motion, active);
            engine_activate_path(ctx, effect, id, loop_id);
            free(loop_id);
        } else {
            char *completed = malloc(strlen(active) + 1);
            strcpy(completed, active);
            free(ch->motion.completed_path);
            ch->motion.completed_path = completed;
            motion_deactivate_path(&ch->motion, active);
            // `active` points into the freed active_path; use the owned copy.
            if (observes_event(ctx, id, EVENT_PATH_COMPLETE)) {
                CallerKey caller = path_caller(completed);
                engine_handle_event(ctx, effect, id, EVENT_PATH_COMPLETE, &caller);
            }
        }
    }
}

// --- animation ------------------------------------------------------------

// Copies the source visual into the character only when the source frame
// changed; retains the boxed value semantics effects rely on.
static void set_current_visual(EngineCtx *ctx, CharId id, VisualHandle src) {
    Animation *anim = &ctx->terminal.arena.items[id].animation;
    if (!src || anim->current_visual == src) {
        return;
    }
    anim->current_visual = src;
    renderer_handle(id, src);
}

void engine_activate_scene(EngineCtx *ctx, Effect *effect, CharId id, const char *scene_id) {
    doze_wake(id);
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    long scene_slot = om_slot(&ch->animation.scenes, scene_id);
    Scene *scene = scene_slot < 0 ? NULL : (Scene *)om_value_at(&ch->animation.scenes, (size_t)scene_slot);
    if (!scene) {
        return;
    }
    VisualHandle src = 0;
    size_t fidx = 0;
    if (scene_activate(scene, &src, &fidx) != 0) {
        return;
    }
    ch->animation.active_scene = strtab_intern(scene_id);
    ch->animation.active_scene_ref = scene;
    set_current_visual(ctx, id, src);
    if (observes_event(ctx, id, EVENT_SCENE_ACTIVATED)) {
        CallerKey key = scene_caller(scene);
        engine_handle_event(ctx, effect, id, EVENT_SCENE_ACTIVATED, &key);
    }
}

void engine_deactivate_scene(EngineCtx *ctx, CharId id, const char *scene_id) {
    doze_wake(id);
    Animation *anim = &ctx->terminal.arena.items[id].animation;
    if (!scene_id) {
        anim->active_scene = NULL;
        anim->active_scene_ref = NULL;
        return;
    }
    if (anim->active_scene && strcmp(anim->active_scene, scene_id) == 0) {
        anim->active_scene = NULL;
        anim->active_scene_ref = NULL;
    }
}

static void complete_scene_if_finished(EngineCtx *ctx, Effect *effect, CharId id, Scene *scene) {
    if (!(scene->head >= scene->all_frames_len || scene->is_looping)) {
        return;
    }
    Animation *anim = &ctx->terminal.arena.items[id].animation;
    if (!scene->is_looping) {
        scene_reset(scene);
        anim->active_scene = NULL;
        anim->active_scene_ref = NULL;
    }
    if (observes_event(ctx, id, EVENT_SCENE_COMPLETE)) {
        CallerKey key = scene_caller(scene);
        engine_handle_event(ctx, effect, id, EVENT_SCENE_COMPLETE, &key);
    }
}

static void step_synced_scene(EngineCtx *ctx, CharId id, Scene *scene, SyncMetric sync) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    bool has_path_state = false;
    int64_t current_step = 0;
    int64_t max_steps = 0;
    double total_distance = 0.0;
    double last_distance_reached = 0.0;
    if (ch->motion.active_path) {
        // The activation-time slot indexes the path directly, so a synced
        // step needs no string lookup; it also follows a path rebuilt under the
        // same name, which a cached pointer would not.
        Path *p = ch->motion.active_path_slot_valid
                      ? (Path *)om_value_at(&ch->motion.paths, ch->motion.active_path_slot)
                      : (Path *)om_get(&ch->motion.paths, ch->motion.active_path);
        if (p) {
            has_path_state = true;
            current_step = p->current_step;
            max_steps = p->max_steps;
            total_distance = p->total_distance;
            last_distance_reached = p->last_distance_reached;
        }
    }
    if (!has_path_state) {
        // No path to track: show the last remaining frame and consume the rest.
        if (scene->head < scene->all_frames_len) {
            set_current_visual(ctx, id, scene->all_frames[scene->all_frames_len - 1].visual);
        }
        scene->head = scene->all_frames_len;
        return;
    }
    int64_t final_frame_index = (int64_t)(scene->all_frames_len - scene->head) - 1;
    double progress_ratio;
    if (sync == SYNC_STEP) {
        int64_t cs = current_step > 1 ? current_step : 1;
        int64_t ms = max_steps > 1 ? max_steps : 1;
        progress_ratio = (double)cs / (double)ms;
    } else {
        double total = total_distance > 1.0 ? total_distance : 1.0;
        double remaining = total_distance - last_distance_reached;
        if (remaining < 1.0) remaining = 1.0;
        double reached = total - remaining;
        if (reached < 1.0) reached = 1.0;
        progress_ratio = reached / total;
    }
    int64_t frame_index = py_round_half_even((double)final_frame_index * progress_ratio);
    if (frame_index > final_frame_index) frame_index = final_frame_index;
    if (frame_index < 0) frame_index = 0;
    if (scene->head + (size_t)frame_index < scene->all_frames_len) {
        set_current_visual(ctx, id, scene->all_frames[scene->head + (size_t)frame_index].visual);
    }
}

static void step_eased_scene(EngineCtx *ctx, CharId id, Scene *scene) {
    const int32_t *shape = scene_easing_shape_seq(scene);
    int64_t frame_index;
    size_t frame = 0;
    int64_t start = 0;
    bool resolved;
    if (shape && scene->easing_current_step < scene->cold->easing_total_steps) {
        int32_t looked = shape[scene->easing_current_step];
        frame = looked < 0 ? 0 : (size_t)looked;
        resolved = looked >= 0 && (size_t)looked < scene->all_frames_len;
    } else {
        frame_index = eased_frame_index(&scene->cold->ease, scene->cold->easing_total_steps,
                                        scene->easing_current_step);
        resolved = scene_frame_at_tick(scene, frame_index, &frame, &start);
    }
    if (resolved) {
        set_current_visual(ctx, id, scene->all_frames[frame].visual);
    }
    scene->easing_current_step += 1;
    if (scene->easing_current_step == scene->cold->easing_total_steps) {
        if (scene->is_looping) {
            scene->easing_current_step = 0;
        } else {
            scene->head = scene->all_frames_len;
        }
    }
}

static void step_animation_now(EngineCtx *ctx, Effect *effect, CharId id, bool may_park) {
    Animation *anim = &ctx->terminal.arena.items[id].animation;
    if (!anim->active_scene) {
        return;
    }
    Scene *scene = anim->active_scene_ref;
    if (!scene) scene = animation_active_scene(anim);
    if (!scene || scene->head >= scene->all_frames_len) {
        return;
    }
    if (scene->has_sync) {
        step_synced_scene(ctx, id, scene, scene->sync);
    } else if (scene->has_ease) {
        step_eased_scene(ctx, id, scene);
    } else {
        set_current_visual(ctx, id, scene_advance(scene));
    }
    complete_scene_if_finished(ctx, effect, id, scene);
    if (may_park) {
        // Completion callbacks can replace the active scene or create a path.
        // Reacquire only after dispatch; the ordinary step uses the hot scene.
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        if (!ch->motion.active_path && ch->animation.active_scene) {
            doze_schedule(&ctx->doze, id, ch->animation.active_scene_ref,
                          ch->animation.current_visual);
        }
    }
}

__attribute__((always_inline))
static inline void tick_now(EngineCtx *ctx, Effect *effect, CharId id) {
    motion_move_now(ctx, effect, id);
    step_animation_now(ctx, effect, id, false);
}

// The public tick entry points serve effects that tick characters themselves;
// a dozing character settles first so it ticks from its true state.
void engine_motion_move(EngineCtx *ctx, Effect *effect, CharId id) {
    doze_wake(id);
    motion_move_now(ctx, effect, id);
}

void engine_step_animation(EngineCtx *ctx, Effect *effect, CharId id) {
    doze_wake(id);
    step_animation_now(ctx, effect, id, false);
}

void engine_tick(EngineCtx *ctx, Effect *effect, CharId id) {
    doze_wake(id);
    tick_now(ctx, effect, id);
}

static bool retain_keep(CharId id, void *ctxp) {
    EngineCtx *ctx = ctxp;
    return character_is_active(&ctx->terminal.arena.items[id]);
}

void engine_bind(EngineCtx *ctx) {
    g_doze = ctx ? &ctx->doze : NULL;
}

__attribute__((always_inline))
static inline void update_slot(EngineCtx *ctx, Effect *effect, size_t w, unsigned b,
                                uint64_t mirrored, uint64_t *pending) {
    Doze *d = &ctx->doze;
    CharId id = (CharId)((w << 6) | b);
    d->cursor = id;
    if (d->bits[w] & (UINT64_C(1) << b)) {
        doze_settle(d, id);
    }
    bool animation_ready = false;
    VisualHandle visual = 0;
    if (mirrored && motion_take_batch(ctx, id, &animation_ready, &visual)) {
        if (animation_ready) {
            set_current_visual(ctx, id, visual);
            const EffectCharacter *done = &ctx->terminal.arena.items[id];
            if (!done->motion.active_path && done->animation.active_scene) {
                doze_schedule(d, id, done->animation.active_scene_ref, visual);
            }
        } else {
            step_animation_now(ctx, effect, id, true);
        }
    } else {
        motion_move_now(ctx, effect, id);
        step_animation_now(ctx, effect, id, true);
    }
    const EffectCharacter *ch = &ctx->terminal.arena.items[id];
    if (!ch->motion.active_path) {
        if (mirrored) {
            uint64_t bit = UINT64_C(1) << b;
            if (ctx->motion_mirrored[w] & bit) {
                ctx->motion_mirrored[w] &= ~bit;
                ctx->mirrors[id].valid = false;
            }
        }
        const Scene *scene = ch->animation.active_scene_ref;
        if (!ch->animation.active_scene || !scene || scene->is_looping ||
            scene->head >= scene->all_frames_len) {
            ac_mark_candidate(&ctx->active_characters, id);
        }
    }
    if (d->snapshot[w]) {
        *pending |= d->snapshot[w];
        d->snapshot[w] = 0;
    }
}

// Keep the idle-word specialization out of the scalar word's inlining budget.
__attribute__((noinline))
static void update_idle_word(EngineCtx *ctx, Effect *effect, size_t w, uint64_t m,
                              uint64_t mirrored, uint64_t idle) {
    m &= ~idle;
    while (m || idle) {
        // A dispatch invalidates remaining prepared lanes, including idle ones.
        if (idle && ctx->motion_batch.epoch != ctx->motion_epoch) {
            m |= idle;
            idle = 0;
        }
        if (idle) {
            uint64_t before = m ? idle & ((m & -m) - 1) : idle;
            idle &= ~before;
            uint64_t retry = motion_retire_idle(ctx, before);
            m |= retry;
            if (retry) continue;
        }
        if (!m) break;
        unsigned b = (unsigned)__builtin_ctzll(m);
        m &= m - 1;
        update_slot(ctx, effect, w, b, mirrored, &m);
    }
}

void engine_update(EngineCtx *ctx, Effect *effect) {
    // Tick a snapshot of the active set in ascending order (ticks can change
    // the live set), leaving out the characters dozing through this update.
    ActiveCharacters *ac = &ctx->active_characters;
    Doze *d = &ctx->doze;
    engine_bind(ctx);
    if (!mirror_grow(ctx)) {
        fputs("glyphfx: out of memory (mirrors)\n", stderr);
        abort();
    }
    if (!doze_grow(d, ctx->terminal.arena.len)) {
        fputs("glyphfx: out of memory (doze)\n", stderr);
        abort();
    }
    uint32_t now = ++d->count;
    // Only the set's window [lo, hi) can hold members (and so dozers).
    size_t lo = ac->lo;
    size_t words = ac->hi;
    if (words > ac->nwords) {
        words = ac->nwords;
    }
    if (words > d->nwords) {
        words = d->nwords;
    }
    if (lo < words) {
        memcpy(d->snapshot + lo, ac->bits + lo, (words - lo) * sizeof(uint64_t));
    }
    for (size_t w = lo; d->live && w < words; w++) {
        uint64_t dozing = d->bits[w];
        if (dozing) {
            d->snapshot[w] &= ~doze_pending_word(d->until + (w << 6), now, dozing);
        }
    }
    d->snapshot_words = words;
    d->in_update = true;
    for (size_t w = lo; w < words; w++) {
        // Pop from a register copy; snapshot[w] then only collects the
        // characters woken ahead of the cursor, which rejoin this word.
        uint64_t m = d->snapshot[w];
        d->snapshot[w] = 0;
        uint64_t mirrored = m & ctx->motion_mirrored[w];
        if (mirrored) {
            motion_prepare_word(ctx, w, mirrored);
        } else {
            ctx->motion_batch.ready = 0;
        }
        uint64_t idle = mirrored ? ctx->motion_batch.idle : 0;
        if (idle) {
            update_idle_word(ctx, effect, w, m, mirrored, idle);
            continue;
        }
        while (m) {
            unsigned b = (unsigned)__builtin_ctzll(m);
            m &= m - 1;
            update_slot(ctx, effect, w, b, mirrored, &m);
        }
    }
    ctx->motion_batch.ready = 0;
    d->in_update = false;
    d->cursor = CHAR_ID_NONE;
    d->snapshot_words = 0;
    // Dozers stay: a doze only covers ticks that leave the character active.
    ac_retain_candidates(ac, d->bits, d->nwords, retain_keep, ctx);
}

const char *engine_frame(EngineCtx *ctx) {
    if (clock_is_real(&ctx->clock) && ctx->terminal.config.frame_rate != 0) {
        terminal_enforce_framerate(&ctx->terminal);
    }
    clock_advance_frame(&ctx->clock);
    if (ctx->terminal.frame_rows_mode) {
        // The run loop emits the rendered rows with writev; the string is only
        // built for the parity and m0 dumps.
        terminal_render_rows(&ctx->terminal);
        return "";
    }
    return terminal_get_formatted_output_string(&ctx->terminal);
}
