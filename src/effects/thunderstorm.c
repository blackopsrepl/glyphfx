// thunderstorm, ported from the reference effects/thunderstorm.rs.
//
// Two ParticlePools (rain + sparks) plus a manually managed strike-character
// pool (available/pending/active lists). The storm clock is read through the
// injected monotonic clock at the upstream points (build, fade_complete,
// next_frame). Rain/spark reclaim-on-event is wired as Callback actions that
// dispatch back into the pools, matching the reference.
#include "effects/thunderstorm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/render_log.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/motion.h"
#include "engine/particles.h"
#include "engine/terminal.h"
#include "utils/easing.h"
#include "utils/graphics.h"
#include "utils/ordmap.h"
#include "utils/pycompat.h"

/// fade_complete: phase -> storm, restart clock.
#define CB_FADE_COMPLETE 0u
/// ThunderstormIterator.hide_character.
#define CB_HIDE_CHARACTER 1u
/// ThunderstormIterator.make_char_glow.
#define CB_MAKE_CHAR_GLOW 2u
/// ThunderstormIterator.return_strike_to_pool.
#define CB_RETURN_STRIKE_TO_POOL 3u
/// ThunderstormIterator.set_strike_in_progress_false.
#define CB_SET_STRIKE_IN_PROGRESS_FALSE 4u
/// rain_pool.reclaim_on_event closure.
#define CB_RECLAIM_RAIN 5u
/// spark_pool.reclaim_on_event closure.
#define CB_RECLAIM_SPARK 6u

typedef enum {
    PHASE_PRE_STORM,
    PHASE_WAITING,
    PHASE_STORM,
    PHASE_COMPLETE,
} Phase;

typedef struct {
    bool has;
    Color color;
} ColorOpt;

typedef struct {
    const Color *colors;
    size_t len;
    int64_t cooling_frames;
} SparkInit;

typedef struct {
    ThunderstormConfig config;
    int64_t delay;
    int64_t strike_progression_delay;
    ParticlePool rain_pool;
    bool has_rain_pool;
    CharId *pending_strike_chars;
    size_t pending_strike_chars_len;
    size_t pending_strike_chars_cap;
    CharId *available_strike_chars;
    size_t available_strike_chars_len;
    size_t available_strike_chars_cap;
    CharId *active_strike_chars;
    size_t active_strike_chars_len;
    size_t active_strike_chars_cap;
    ParticlePool spark_pool;
    bool has_spark_pool;
    Gradient spark_gradient;
    bool has_spark_gradient;
    CharId *pending_glow_chars;
    size_t pending_glow_chars_len;
    size_t pending_glow_chars_cap;
    bool strike_in_progress;
    double strike_branch_chance;
    Phase phase;
    double storm_start_time;
} Thunderstorm;

// --- small helpers --------------------------------------------------------

static char *dup_cstr(const char *s) {
    if (!s) {
        s = "";
    }
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (out) {
        memcpy(out, s, n + 1);
    }
    return out;
}

static Easing no_ease(void) {
    Easing e;
    memset(&e, 0, sizeof(e));
    return e;
}

static void idvec_push(CharId **items, size_t *len, size_t *cap, CharId id) {
    if (*len == *cap) {
        size_t c = *cap ? *cap * 2 : 8;
        CharId *grown = realloc(*items, c * sizeof(CharId));
        if (!grown) {
            return;
        }
        *items = grown;
        *cap = c;
    }
    (*items)[(*len)++] = id;
}

static void idvec_remove_front(CharId *items, size_t *len) {
    if (*len > 1) {
        memmove(&items[0], &items[1], (*len - 1) * sizeof(CharId));
    }
    if (*len > 0) {
        (*len)--;
    }
}

static bool colorpair_eq(const ColorPair *a, const ColorPair *b) {
    if (a->has_fg != b->has_fg) {
        return false;
    }
    if (a->has_fg && !color_eq(&a->fg, &b->fg)) {
        return false;
    }
    if (a->has_bg != b->has_bg) {
        return false;
    }
    if (a->has_bg && !color_eq(&a->bg, &b->bg)) {
        return false;
    }
    return true;
}

static void push_color_default(ColorList *list, const char *hex) {
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 4;
        Color *grown = realloc(list->items, cap * sizeof(Color));
        if (!grown) {
            return;
        }
        list->items = grown;
        list->cap = cap;
    }
    Color c;
    color_from_hex(hex, &c);
    list->items[list->len++] = c;
}

static void push_int_default(IntList *list, int64_t v) {
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 4;
        int64_t *grown = realloc(list->items, cap * sizeof(int64_t));
        if (!grown) {
            return;
        }
        list->items = grown;
        list->cap = cap;
    }
    list->items[list->len++] = v;
}

static void push_symbol_default(StringList *list, const char *symbol) {
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 4;
        char **grown = realloc(list->items, cap * sizeof(char *));
        if (!grown) {
            return;
        }
        list->items = grown;
        list->cap = cap;
    }
    char *copy = malloc(strlen(symbol) + 1);
    strcpy(copy, symbol);
    list->items[list->len++] = copy;
}

// --- config ---------------------------------------------------------------

void thunderstorm_config_defaults(void *cfg_ptr) {
    ThunderstormConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    color_from_hex("68A3E8", &cfg->lightning_color);
    color_from_hex("EF5411", &cfg->glowing_text_color);
    cfg->text_glow_time = 6;
    push_symbol_default(&cfg->raindrop_symbols, "\\");
    push_symbol_default(&cfg->raindrop_symbols, ".");
    push_symbol_default(&cfg->raindrop_symbols, ",");
    push_symbol_default(&cfg->spark_symbols, "*");
    push_symbol_default(&cfg->spark_symbols, ".");
    push_symbol_default(&cfg->spark_symbols, "'");
    color_from_hex("ff4d00", &cfg->spark_glow_color);
    cfg->spark_glow_time = 18;
    cfg->storm_time = 12;
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "FFFFFF");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_frames = 3;
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void thunderstorm_free_config(void *cfg_ptr) {
    ThunderstormConfig *cfg = cfg_ptr;
    for (size_t i = 0; i < cfg->raindrop_symbols.len; i++) {
        free(cfg->raindrop_symbols.items[i]);
    }
    free(cfg->raindrop_symbols.items);
    for (size_t i = 0; i < cfg->spark_symbols.len; i++) {
        free(cfg->spark_symbols.items[i]);
    }
    free(cfg->spark_symbols.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

// --- shared computations --------------------------------------------------

/// ThunderstormIterator._adjust_color_pair_brightness.
static ColorPair adjust_color_pair_brightness(const ColorPair *colors, double brightness) {
    ColorPair out;
    memset(&out, 0, sizeof(out));
    if (colors->has_fg) {
        out.has_fg = true;
        out.fg = color_adjust_brightness(&colors->fg, brightness);
    }
    if (colors->has_bg) {
        out.has_bg = true;
        out.bg = color_adjust_brightness(&colors->bg, brightness);
    }
    return out;
}

/// ThunderstormIterator._add_color_pair_gradient_frames. Faithful quirk: when
/// both endpoint colors exist the gradient list has steps+1 entries but only
/// range(steps) of them are emitted as frames.
static int add_color_pair_gradient_frames(Scene *scene, const char *symbol, const ColorPair *start_colors,
                                          const ColorPair *end_colors, int64_t steps, int64_t duration) {
    if (steps < 1) {
        return -1;
    }
    size_t slot_count = (size_t)steps + 1;
    ColorOpt *fg_steps = calloc(slot_count, sizeof(ColorOpt));
    ColorOpt *bg_steps = calloc(slot_count, sizeof(ColorOpt));
    if (!fg_steps || !bg_steps) {
        free(fg_steps);
        free(bg_steps);
        return -1;
    }
    if (start_colors->has_fg && end_colors->has_fg) {
        Color stops[2] = {start_colors->fg, end_colors->fg};
        Gradient g;
        if (gradient_with_steps(stops, 2, steps, false, &g) != 0) {
            free(fg_steps);
            free(bg_steps);
            return -1;
        }
        for (size_t i = 0; i < g.len; i++) {
            fg_steps[i].has = true;
            fg_steps[i].color = g.spectrum[i];
        }
        gradient_free(&g);
    } else {
        ColorOpt filler;
        memset(&filler, 0, sizeof(filler));
        if (end_colors->has_fg) {
            filler.has = true;
            filler.color = end_colors->fg;
        } else if (start_colors->has_fg) {
            filler.has = true;
            filler.color = start_colors->fg;
        }
        for (int64_t i = 0; i < steps; i++) {
            fg_steps[i] = filler;
        }
    }
    if (start_colors->has_bg && end_colors->has_bg) {
        Color stops[2] = {start_colors->bg, end_colors->bg};
        Gradient g;
        if (gradient_with_steps(stops, 2, steps, false, &g) != 0) {
            free(fg_steps);
            free(bg_steps);
            return -1;
        }
        for (size_t i = 0; i < g.len; i++) {
            bg_steps[i].has = true;
            bg_steps[i].color = g.spectrum[i];
        }
        gradient_free(&g);
    } else {
        ColorOpt filler;
        memset(&filler, 0, sizeof(filler));
        if (end_colors->has_bg) {
            filler.has = true;
            filler.color = end_colors->bg;
        } else if (start_colors->has_bg) {
            filler.has = true;
            filler.color = start_colors->bg;
        }
        for (int64_t i = 0; i < steps; i++) {
            bg_steps[i] = filler;
        }
    }
    int rc = 0;
    for (int64_t index = 0; index < steps && rc == 0; index++) {
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors.has_fg = fg_steps[index].has;
        if (fg_steps[index].has) {
            vp.colors.fg = fg_steps[index].color;
        }
        vp.colors.has_bg = bg_steps[index].has;
        if (bg_steps[index].has) {
            vp.colors.bg = bg_steps[index].color;
        }
        if (!scene || scene_add_frame(scene, symbol, duration, &vp) != 0) {
            rc = -1;
        }
    }
    free(fg_steps);
    free(bg_steps);
    return rc;
}

// --- particle initializers / path setups ----------------------------------

/// build_rain_pool's initialize_raindrop.
static void initialize_raindrop(void *user, EngineCtx *ctx, CharId id) {
    (void)user;
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    ch->layer = 1;
    renderer_layer(id, 1);
    char *input_symbol = dup_cstr(ch->input_symbol);
    bool uses_pre = ch->uses_input_preexisting_colors;
    ColorPair colors;
    memset(&colors, 0, sizeof(colors));
    colors.has_fg = true;
    color_from_hex("aaaaff", &colors.fg);
    animation_set_appearance(&ctx->terminal.arena.items[id].animation, uses_pre, input_symbol, &colors);
    free(input_symbol);
}

/// build_spark_pool's _build_spark_characters.
static void initialize_spark(void *user, EngineCtx *ctx, CharId id) {
    SparkInit *init = user;
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    ch->layer = 2;
    renderer_layer(id, 2);
    char *input_symbol = dup_cstr(ch->input_symbol);
    bool uses_pre = ch->uses_input_preexisting_colors;
    Easing ease = easing_named(EASE_IN_CIRC);
    const char *spark_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                SYNC_DISTANCE, true, ease, "glow", uses_pre);
    Scene *scene = spark_scn ? (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, spark_scn) : NULL;
    if (scene) {
        for (size_t i = 0; i < init->len; i++) {
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = init->colors[i];
            scene_add_frame(scene, input_symbol, init->cooling_frames, &vp);
        }
    }
    free(input_symbol);
}

/// ThunderstormIterator._setup_raindrop.
static void setup_raindrop(void *user, EngineCtx *ctx, CharId id) {
    Effect *self = user;
    Coord origin = ctx->terminal.arena.items[id].motion.current_coord;
    double speed = rng_uniform(&ctx->rng, 0.5, 1.5);
    int64_t canvas_top = ctx->terminal.canvas.top;
    int64_t canvas_bottom = ctx->terminal.canvas.bottom;
    char *fall_path = NULL;
    if (motion_new_path(&ctx->terminal.arena.items[id].motion, speed, false, no_ease(), false, 0, 0, false, "",
                        &fall_path) != 0) {
        return;
    }
    Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, fall_path);
    Waypoint wp;
    memset(&wp, 0, sizeof(wp));
    (void)path_new_waypoint(p, coord_new(origin.column + canvas_top + 1, canvas_bottom - 1), NULL, 0, "", &wp);
    waypoint_free(&wp);

    CallerKey caller;
    memset(&caller, 0, sizeof(caller));
    caller.kind = CALLER_PATH;
    caller.id = fall_path;
    EventAction action;
    memset(&action, 0, sizeof(action));
    action.kind = ACTION_CALLBACK;
    action.cb.id = CB_RECLAIM_RAIN;
    (void)engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action);
    // PATH_ACTIVATED has no registered handlers on the raindrop, so passing
    // the effect is observably identical to the reference's NoopHooks.
    engine_activate_path(ctx, self, id, fall_path);
    free(fall_path);
}

/// ThunderstormIterator._setup_sparks_for_impact.
static void setup_sparks_for_impact(void *user, EngineCtx *ctx, CharId id) {
    Effect *self = user;
    Coord impact_coord = ctx->terminal.arena.items[id].motion.current_coord;
    double speed = rng_uniform(&ctx->rng, 0.1, 0.25);
    char *spark_path = NULL;
    if (motion_new_path(&ctx->terminal.arena.items[id].motion, speed, true, easing_named(EASE_OUT_QUINT), false, 0,
                        30, false, "", &spark_path) != 0) {
        return;
    }
    int64_t magnitude = rng_randint(&ctx->rng, 4, 20);
    int64_t sign = rng_choice_index(&ctx->rng, 2) == 0 ? 1 : -1;
    int64_t offset = magnitude * sign;
    Coord spark_target = coord_new(impact_coord.column + offset, ctx->terminal.canvas.bottom);
    int64_t bezier_column = impact_coord.column - py_floor_div(impact_coord.column - spark_target.column, 2);
    int64_t bezier_row = rng_randint(&ctx->rng, 1, ctx->terminal.canvas.top);
    Coord bezier = coord_new(bezier_column, bezier_row);
    Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, spark_path);
    Waypoint wp;
    memset(&wp, 0, sizeof(wp));
    (void)path_new_waypoint(p, spark_target, &bezier, 1, "", &wp);
    waypoint_free(&wp);

    CallerKey caller;
    memset(&caller, 0, sizeof(caller));
    caller.kind = CALLER_SCENE;
    caller.id = "glow";
    EventAction action;
    memset(&action, 0, sizeof(action));
    action.kind = ACTION_CALLBACK;
    action.cb.id = CB_RECLAIM_SPARK;
    (void)engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action);
    engine_activate_scene(ctx, self, id, "glow");
    engine_activate_path(ctx, self, id, spark_path);
    free(spark_path);
}

// --- strike pool ----------------------------------------------------------

/// ThunderstormIterator.build_strike_characters.
static void build_strike_characters(Effect *self, EngineCtx *ctx, size_t count) {
    Thunderstorm *st = self->state;
    for (size_t i = 0; i < count; i++) {
        CharId strike_char = terminal_add_character(&ctx->terminal, "|", coord_new(1, 1));
        idvec_push(&st->available_strike_chars, &st->available_strike_chars_len,
                   &st->available_strike_chars_cap, strike_char);
    }
}

/// ThunderstormIterator.get_next_strike_char.
static CharId get_next_strike_char(Effect *self, EngineCtx *ctx) {
    Thunderstorm *st = self->state;
    if (st->available_strike_chars_len == 0) {
        build_strike_characters(self, ctx, 20);
    }
    CharId strike_char = st->available_strike_chars[--st->available_strike_chars_len];
    EffectCharacter *ch = &ctx->terminal.arena.items[strike_char];
    animation_clear_scenes(&ch->animation);
    event_handler_clear(&ch->event_handler);
    return strike_char;
}

/// ThunderstormIterator.setup_lightning_strike (recursive branching).
static void setup_lightning_strike(Effect *self, EngineCtx *ctx, CharId branch_neighbor) {
    Thunderstorm *st = self->state;
    bool has_branch = branch_neighbor != CHAR_ID_NONE;
    int64_t column;
    int64_t row;
    if (has_branch) {
        Coord coord = ctx->terminal.arena.items[branch_neighbor].motion.current_coord;
        column = coord.column;
        row = coord.row;
    } else {
        column = rng_randint(&ctx->rng, 1, ctx->terminal.canvas.right);
        row = ctx->terminal.canvas.top;
    }

    while (row >= ctx->terminal.canvas.bottom) {
        if (st->available_strike_chars_len == 0) {
            build_strike_characters(self, ctx, 20);
        }
        const char *symbol;
        if (has_branch) {
            // Strike characters are all created with input_symbol "|", so the
            // "/" and "\\" arms are unreachable upstream too — transcribed
            // faithfully anyway.
            const char *neighbor_symbol = ctx->terminal.arena.items[branch_neighbor].input_symbol;
            if (strcmp(neighbor_symbol, "/") == 0) {
                column += 1;
                static const char *const choices[2] = {"|", "\\"};
                symbol = choices[rng_choice_index(&ctx->rng, 2)];
            } else if (strcmp(neighbor_symbol, "\\") == 0) {
                column -= 1;
                static const char *const choices[2] = {"|", "/"};
                symbol = choices[rng_choice_index(&ctx->rng, 2)];
            } else {
                static const int64_t deltas[2] = {-1, 1};
                int64_t delta = deltas[rng_choice_index(&ctx->rng, 2)];
                column += delta;
                symbol = delta == 1 ? "\\" : "/";
            }
        } else {
            static const char *const choices[3] = {"\\", "/", "|"};
            symbol = choices[rng_choice_index(&ctx->rng, 3)];
        }

        CharId strike_char = get_next_strike_char(self, ctx);
        {
            EffectCharacter *ch = &ctx->terminal.arena.items[strike_char];
            motion_set_coordinate(&ch->motion, coord_new(column, row));
            char *input_symbol = dup_cstr(ch->input_symbol);
            bool uses_pre = ch->uses_input_preexisting_colors;
            ColorPair colors;
            memset(&colors, 0, sizeof(colors));
            colors.has_fg = true;
            colors.fg = st->config.lightning_color;
            animation_set_appearance(&ctx->terminal.arena.items[strike_char].animation, uses_pre, symbol, &colors);
            free(input_symbol);
        }
        row -= 1;
        if (strcmp(symbol, "\\") == 0) {
            column += 1;
        } else if (strcmp(symbol, "/") == 0) {
            column -= 1;
        }

        idvec_push(&st->pending_strike_chars, &st->pending_strike_chars_len, &st->pending_strike_chars_cap,
                   strike_char);
        // random.random() is always drawn (left operand of `and`).
        if (rng_random(&ctx->rng) < st->strike_branch_chance && !has_branch) {
            st->strike_branch_chance -= 0.01;
            setup_lightning_strike(self, ctx, strike_char);
        }
        has_branch = false;
        branch_neighbor = CHAR_ID_NONE;
    }
    st->strike_branch_chance = 0.05;
}

/// ThunderstormIterator.lightning_strike.
static void lightning_strike(Effect *self, EngineCtx *ctx) {
    Thunderstorm *st = self->state;
    setup_lightning_strike(self, ctx, CHAR_ID_NONE);

    Color strike_base_color = st->config.lightning_color;
    Color strike_flash_color = color_adjust_brightness(&strike_base_color, 1.7);
    Color strike_stops[2] = {strike_base_color, strike_flash_color};
    Gradient strike_gradient;
    if (gradient_with_steps(strike_stops, 2, 7, true, &strike_gradient) != 0) {
        return;
    }
    Color fade_stops[2] = {strike_base_color, ctx->terminal.config.terminal_background_color};
    Gradient fade_gradient;
    if (gradient_with_steps(fade_stops, 2, 6, false, &fade_gradient) != 0) {
        gradient_free(&strike_gradient);
        return;
    }
    Easing flash_ease = easing_cubic_bezier(0.0, 1.6, 1.0, rng_uniform(&ctx->rng, -0.6, 0.4));

    for (size_t i = 0; i < st->pending_strike_chars_len; i++) {
        CharId strike_char = st->pending_strike_chars[i];
        char *symbol = dup_cstr(ctx->terminal.arena.items[strike_char].animation.current_visual->symbol);
        bool uses_pre = ctx->terminal.arena.items[strike_char].uses_input_preexisting_colors;

        const char *flash_scn = animation_new_scene(&ctx->terminal.arena.items[strike_char].animation, false, false,
                                                    SYNC_DISTANCE, true, flash_ease, "flash", uses_pre);
        Scene *scene = flash_scn
                           ? (Scene *)om_get(&ctx->terminal.arena.items[strike_char].animation.scenes, flash_scn)
                           : NULL;
        for (size_t j = 0; j < strike_gradient.len; j++) {
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = strike_gradient.spectrum[j];
            scene_add_frame(scene, symbol, 6, &vp);
        }
        const char *fade_scn = animation_new_scene(&ctx->terminal.arena.items[strike_char].animation, false, false,
                                                   SYNC_DISTANCE, false, no_ease(), "fade", uses_pre);
        scene = fade_scn ? (Scene *)om_get(&ctx->terminal.arena.items[strike_char].animation.scenes, fade_scn) : NULL;
        for (size_t j = 0; j < fade_gradient.len; j++) {
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = fade_gradient.spectrum[j];
            scene_add_frame(scene, symbol, 2, &vp);
        }
        ctx->terminal.arena.items[strike_char].layer = 1;
        renderer_layer(strike_char, 1);

        CallerKey flash_caller;
        memset(&flash_caller, 0, sizeof(flash_caller));
        flash_caller.kind = CALLER_SCENE;
        flash_caller.id = "flash";
        EventAction action;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_SCENE;
        action.id = "fade";
        action.has_id = true;
        (void)engine_register_event(ctx, strike_char, EVENT_SCENE_COMPLETE, &flash_caller, &action);

        CallerKey fade_caller;
        memset(&fade_caller, 0, sizeof(fade_caller));
        fade_caller.kind = CALLER_SCENE;
        fade_caller.id = "fade";
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_CALLBACK;
        action.cb.id = CB_HIDE_CHARACTER;
        (void)engine_register_event(ctx, strike_char, EVENT_SCENE_COMPLETE, &fade_caller, &action);
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_CALLBACK;
        action.cb.id = CB_MAKE_CHAR_GLOW;
        (void)engine_register_event(ctx, strike_char, EVENT_SCENE_COMPLETE, &fade_caller, &action);
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_CALLBACK;
        action.cb.id = CB_RETURN_STRIKE_TO_POOL;
        (void)engine_register_event(ctx, strike_char, EVENT_SCENE_COMPLETE, &fade_caller, &action);

        free(symbol);
    }

    size_t text_len = 0;
    CharId *text_chars = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &text_len);
    for (size_t i = 0; i < text_len; i++) {
        Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[text_chars[i]].animation.scenes, "flash");
        if (scene) {
            scene->has_ease = true;
            scene->ease = flash_ease;
        }
    }
    free(text_chars);
    gradient_free(&strike_gradient);
    gradient_free(&fade_gradient);
}

/// ThunderstormIterator.step_lightning_strike.
static void step_lightning_strike(Effect *self, EngineCtx *ctx) {
    Thunderstorm *st = self->state;
    if (st->strike_progression_delay != 0) {
        st->strike_progression_delay -= 1;
        return;
    }
    if (st->pending_strike_chars_len == 0) {
        return;
    }
    int64_t batch = rng_randint(&ctx->rng, 1, 3);
    for (int64_t b = 0; b < batch; b++) {
        if (st->pending_strike_chars_len == 0) {
            break;
        }
        CharId next_strike_char = st->pending_strike_chars[0];
        idvec_remove_front(st->pending_strike_chars, &st->pending_strike_chars_len);
        idvec_push(&st->active_strike_chars, &st->active_strike_chars_len, &st->active_strike_chars_cap,
                   next_strike_char);
        terminal_set_character_visibility(&ctx->terminal, next_strike_char, true);
        st->strike_progression_delay = 1;

        // if the last strike_char was activated, activate the sparks and setup
        // the post-fade callback to indicate the strike has ended.
        if (st->pending_strike_chars_len == 0) {
            int64_t spark_count = rng_randint(&ctx->rng, 12, 18);
            for (int64_t s = 0; s < spark_count; s++) {
                CharId last = st->active_strike_chars[st->active_strike_chars_len - 1];
                Coord origin = ctx->terminal.arena.items[last].motion.current_coord;
                SparkInit init;
                init.colors = st->spark_gradient.spectrum;
                init.len = st->spark_gradient.len;
                init.cooling_frames = st->config.spark_glow_time;
                ParticleReset reset = particle_reset_default();
                reset.clear_events = true;
                particle_pool_emit(&st->spark_pool, ctx, origin, NULL, true, reset, initialize_spark, &init,
                                   setup_sparks_for_impact, self);
            }
            CallerKey caller;
            memset(&caller, 0, sizeof(caller));
            caller.kind = CALLER_SCENE;
            caller.id = "fade";
            EventAction action;
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_CALLBACK;
            action.cb.id = CB_SET_STRIKE_IN_PROGRESS_FALSE;
            (void)engine_register_event(ctx, next_strike_char, EVENT_SCENE_COMPLETE, &caller, &action);

            // activate the flash scene on all strike chars and text
            size_t strikes_len = st->active_strike_chars_len;
            for (size_t i = 0; i < strikes_len; i++) {
                CharId strike_char = st->active_strike_chars[i];
                engine_activate_scene(ctx, self, strike_char, "flash");
                ac_insert(&ctx->active_characters, strike_char);
            }
            st->active_strike_chars_len = 0;

            size_t text_len = 0;
            CharId *text_chars = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                         CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &text_len);
            for (size_t i = 0; i < text_len; i++) {
                engine_activate_scene(ctx, self, text_chars[i], "flash");
                ac_insert(&ctx->active_characters, text_chars[i]);
            }
            free(text_chars);
        }
    }
}

/// ThunderstormIterator.rain.
static void thunderstorm_rain(Effect *self, EngineCtx *ctx) {
    Thunderstorm *st = self->state;
    if (st->delay != 0) {
        st->delay -= 1;
        return;
    }
    int64_t count = rng_randint(&ctx->rng, 1, 6);
    for (int64_t i = 0; i < count; i++) {
        int64_t spawn_column = rng_randint(&ctx->rng, 1 - ctx->terminal.canvas.top, ctx->terminal.canvas.right);
        Coord origin = coord_new(spawn_column - 1, ctx->terminal.canvas.top + 1);
        ParticleReset reset = particle_reset_default();
        reset.clear_events = true;
        particle_pool_emit(&st->rain_pool, ctx, origin, NULL, true, reset, initialize_raindrop, NULL, setup_raindrop,
                           self);
    }
    st->delay = rng_randint(&ctx->rng, 1, 7);
}

/// pre_storm_text_fade / post_storm_text_fade_in.
static void thunderstorm_text_scene(Effect *self, EngineCtx *ctx, const char *scene_id) {
    size_t len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &len);
    for (size_t i = 0; i < len; i++) {
        engine_activate_scene(ctx, self, characters[i], scene_id);
        ac_insert(&ctx->active_characters, characters[i]);
    }
    free(characters);
}

// --- effect lifecycle -----------------------------------------------------

static int thunderstorm_build(Effect *self, EngineCtx *ctx) {
    Thunderstorm *st = self->state;
    ThunderstormConfig *cfg = &st->config;

    // __init__ preamble: pools preallocated before build() runs, storm clock
    // read once.
    if (particle_pool_preallocate(&st->rain_pool, ctx, 50, initialize_raindrop, NULL) != 0) {
        return -1;
    }
    Color spark_stops[2] = {cfg->spark_glow_color, ctx->terminal.config.terminal_background_color};
    if (gradient_with_steps(spark_stops, 2, 7, false, &st->spark_gradient) != 0) {
        return -1;
    }
    st->has_spark_gradient = true;
    {
        SparkInit init;
        init.colors = st->spark_gradient.spectrum;
        init.len = st->spark_gradient.len;
        init.cooling_frames = cfg->spark_glow_time;
        if (particle_pool_preallocate(&st->spark_pool, ctx, 200, initialize_spark, &init) != 0) {
            return -1;
        }
    }
    st->storm_start_time = clock_now_monotonic(&ctx->clock);

    // build() body
    Gradient final_gradient;
    if (gradient_new(cfg->final_gradient_stops.items, cfg->final_gradient_stops.len, cfg->final_gradient_steps.items,
                     cfg->final_gradient_steps.len, false, false, &final_gradient) != 0) {
        return -1;
    }
    CoordColorMap final_gradient_mapping;
    if (gradient_build_coordinate_color_mapping(&final_gradient, ctx->terminal.canvas.text_bottom,
                                                ctx->terminal.canvas.text_top, ctx->terminal.canvas.text_left,
                                                ctx->terminal.canvas.text_right, cfg->final_gradient_direction,
                                                &final_gradient_mapping) != 0) {
        gradient_free(&final_gradient);
        return -1;
    }
    build_strike_characters(self, ctx, 200);

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    Color dynamic_neutral_gray;
    color_from_hex("808080", &dynamic_neutral_gray);
    Easing ease = no_ease();

    size_t all_len = 0;
    CharId *all_chars = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &all_len);
    int rc = 0;
    for (size_t i = 0; i < all_len && rc == 0; i++) {
        CharId id = all_chars[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        char *input_symbol = dup_cstr(ch->input_symbol);
        Coord input_coord = ch->input_coord;
        bool uses_pre = ch->uses_input_preexisting_colors;
        bool has_input_fg = ch->animation.has_input_fg;
        Color input_fg = ch->animation.input_fg_color;
        bool has_input_bg = ch->animation.has_input_bg;
        Color input_bg = ch->animation.input_bg_color;

        ColorPair visible_colors;
        ColorPair restore_colors;
        memset(&visible_colors, 0, sizeof(visible_colors));
        memset(&restore_colors, 0, sizeof(restore_colors));
        if (dynamic) {
            visible_colors.has_fg = true;
            visible_colors.fg = has_input_fg ? input_fg : dynamic_neutral_gray;
            visible_colors.has_bg = has_input_bg;
            visible_colors.bg = input_bg;
            restore_colors.has_fg = has_input_fg;
            restore_colors.fg = input_fg;
            restore_colors.has_bg = has_input_bg;
            restore_colors.bg = input_bg;
        } else {
            const Color *mapped = coordcolormap_get(&final_gradient_mapping, input_coord);
            visible_colors.has_fg = true;
            if (mapped) {
                visible_colors.fg = *mapped;
            }
            restore_colors = visible_colors;
        }
        ColorPair storm_colors = adjust_color_pair_brightness(&visible_colors, 0.5);

        // post-strike glow and cool scene
        Color glow_stops[2] = {cfg->glowing_text_color, storm_colors.fg};
        Gradient glow_fg_gradient;
        if (gradient_with_steps(glow_stops, 2, 7, false, &glow_fg_gradient) != 0) {
            free(input_symbol);
            rc = -1;
            break;
        }
        const char *glow_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                   SYNC_DISTANCE, false, ease, "glow", uses_pre);
        Scene *glow_scene =
            glow_scn ? (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, glow_scn) : NULL;
        for (size_t j = 0; j < glow_fg_gradient.len && rc == 0; j++) {
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = glow_fg_gradient.spectrum[j];
            vp.colors.has_bg = storm_colors.has_bg;
            if (storm_colors.has_bg) {
                vp.colors.bg = storm_colors.bg;
            }
            if (!glow_scene || scene_add_frame(glow_scene, input_symbol, cfg->text_glow_time, &vp) != 0) {
                rc = -1;
            }
        }
        if (rc == 0 && dynamic) {
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors = storm_colors;
            if (!glow_scene || scene_add_frame(glow_scene, input_symbol, cfg->text_glow_time, &vp) != 0) {
                rc = -1;
            }
        }
        gradient_free(&glow_fg_gradient);

        // fade before storm scene
        const char *fade_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                   SYNC_DISTANCE, false, ease, "fade", uses_pre);
        Scene *fade_scene =
            fade_scn ? (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, fade_scn) : NULL;
        if (rc == 0) {
            if (dynamic) {
                if (add_color_pair_gradient_frames(fade_scene, input_symbol, &visible_colors, &storm_colors, 7, 12) !=
                    0) {
                    rc = -1;
                }
                if (rc == 0) {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    vp.colors = storm_colors;
                    if (!fade_scene || scene_add_frame(fade_scene, input_symbol, 12, &vp) != 0) {
                        rc = -1;
                    }
                }
            } else {
                Color stops[2] = {visible_colors.fg, storm_colors.fg};
                Gradient fade_gradient;
                if (gradient_with_steps(stops, 2, 7, false, &fade_gradient) != 0) {
                    rc = -1;
                } else {
                    for (size_t j = 0; j < fade_gradient.len && rc == 0; j++) {
                        VisualParams vp;
                        memset(&vp, 0, sizeof(vp));
                        vp.has_colors = true;
                        vp.colors.has_fg = true;
                        vp.colors.fg = fade_gradient.spectrum[j];
                        if (!fade_scene || scene_add_frame(fade_scene, input_symbol, 12, &vp) != 0) {
                            rc = -1;
                        }
                    }
                    gradient_free(&fade_gradient);
                }
            }
        }

        // unfade scene
        const char *unfade_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                     SYNC_DISTANCE, false, ease, "unfade", uses_pre);
        Scene *unfade_scene =
            unfade_scn ? (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, unfade_scn) : NULL;
        if (rc == 0) {
            if (dynamic) {
                if (add_color_pair_gradient_frames(unfade_scene, input_symbol, &storm_colors, &visible_colors, 7,
                                                   12) != 0) {
                    rc = -1;
                }
                if (rc == 0) {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    vp.colors = visible_colors;
                    if (!unfade_scene || scene_add_frame(unfade_scene, input_symbol, 12, &vp) != 0) {
                        rc = -1;
                    }
                }
                if (rc == 0 && !colorpair_eq(&restore_colors, &visible_colors)) {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    vp.colors = restore_colors;
                    if (!unfade_scene || scene_add_frame(unfade_scene, input_symbol, 12, &vp) != 0) {
                        rc = -1;
                    }
                }
            } else {
                Color stops[2] = {visible_colors.fg, storm_colors.fg};
                Gradient unfade_gradient;
                if (gradient_with_steps(stops, 2, 7, false, &unfade_gradient) != 0) {
                    rc = -1;
                } else {
                    for (size_t j = unfade_gradient.len; j > 0 && rc == 0; j--) {
                        VisualParams vp;
                        memset(&vp, 0, sizeof(vp));
                        vp.has_colors = true;
                        vp.colors.has_fg = true;
                        vp.colors.fg = unfade_gradient.spectrum[j - 1];
                        if (!unfade_scene || scene_add_frame(unfade_scene, input_symbol, 12, &vp) != 0) {
                            rc = -1;
                        }
                    }
                    gradient_free(&unfade_gradient);
                }
            }
        }

        // lightning flash scene
        Color lightning_flash_color = color_adjust_brightness(&visible_colors.fg, 1.7);
        Color flash_stops[2] = {storm_colors.fg, lightning_flash_color};
        Gradient flash_gradient;
        if (rc == 0 && gradient_with_steps(flash_stops, 2, 7, true, &flash_gradient) != 0) {
            rc = -1;
        }
        if (rc == 0) {
            const char *strike_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                         SYNC_DISTANCE, false, ease, "flash", uses_pre);
            Scene *strike_scene =
                strike_scn ? (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, strike_scn) : NULL;
            for (size_t j = 0; j < flash_gradient.len && rc == 0; j++) {
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                vp.colors.has_fg = true;
                vp.colors.fg = flash_gradient.spectrum[j];
                vp.colors.has_bg = storm_colors.has_bg;
                if (storm_colors.has_bg) {
                    vp.colors.bg = storm_colors.bg;
                }
                if (!strike_scene || scene_add_frame(strike_scene, input_symbol, 6, &vp) != 0) {
                    rc = -1;
                }
            }
            gradient_free(&flash_gradient);
        }

        if (rc == 0) {
            terminal_set_character_visibility(&ctx->terminal, id, true);
        }
        free(input_symbol);
    }

    // reference character callback: signals the pre-storm fade completed
    if (rc == 0 && all_len > 0) {
        CallerKey caller;
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_SCENE;
        caller.id = "fade";
        EventAction action;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_CALLBACK;
        action.cb.id = CB_FADE_COMPLETE;
        if (engine_register_event(ctx, all_chars[0], EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
            rc = -1;
        }
    }

    free(all_chars);
    coordcolormap_free(&final_gradient_mapping);
    gradient_free(&final_gradient);
    return rc;
}

static const char *thunderstorm_next_frame(Effect *self, EngineCtx *ctx) {
    Thunderstorm *st = self->state;
    if (ac_is_empty(&ctx->active_characters) && st->phase == PHASE_COMPLETE) {
        return NULL;
    }
    switch (st->phase) {
        case PHASE_PRE_STORM:
            thunderstorm_text_scene(self, ctx, "fade");
            st->phase = PHASE_WAITING;
            break;
        case PHASE_STORM:
            thunderstorm_rain(self, ctx);
            if (!st->strike_in_progress && rng_random(&ctx->rng) < 0.008) {
                st->strike_in_progress = true;
                lightning_strike(self, ctx);
            }
            if (st->strike_in_progress) {
                step_lightning_strike(self, ctx);
            }
            for (size_t i = 0; i < st->pending_glow_chars_len; i++) {
                ac_insert(&ctx->active_characters, st->pending_glow_chars[i]);
            }
            st->pending_glow_chars_len = 0;
            if (clock_now_monotonic(&ctx->clock) - st->storm_start_time >= (double)st->config.storm_time &&
                !st->strike_in_progress) {
                thunderstorm_text_scene(self, ctx, "unfade");
                st->phase = PHASE_COMPLETE;
            }
            break;
        case PHASE_WAITING:
        case PHASE_COMPLETE:
            break;
    }
    engine_update(ctx, self);
    return engine_frame(ctx);
}

static void thunderstorm_callback(Effect *self, EngineCtx *ctx, CharId character, const EffectCallback *cb) {
    Thunderstorm *st = self->state;
    switch (cb->id) {
        case CB_FADE_COMPLETE:
            st->phase = PHASE_STORM;
            st->storm_start_time = clock_now_monotonic(&ctx->clock);
            break;
        case CB_HIDE_CHARACTER:
            terminal_set_character_visibility(&ctx->terminal, character, false);
            break;
        case CB_MAKE_CHAR_GLOW: {
            Coord coord = ctx->terminal.arena.items[character].motion.current_coord;
            CharId input_char = terminal_get_character_by_input_coord(&ctx->terminal, coord);
            if (input_char != CHAR_ID_NONE && ctx->terminal.arena.items[input_char].is_visible) {
                engine_activate_scene(ctx, self, input_char, "glow");
                idvec_push(&st->pending_glow_chars, &st->pending_glow_chars_len, &st->pending_glow_chars_cap,
                           input_char);
            }
            break;
        }
        case CB_RETURN_STRIKE_TO_POOL:
            idvec_push(&st->available_strike_chars, &st->available_strike_chars_len,
                       &st->available_strike_chars_cap, character);
            break;
        case CB_SET_STRIKE_IN_PROGRESS_FALSE:
            st->strike_in_progress = false;
            break;
        case CB_RECLAIM_RAIN:
            particle_pool_reclaim(&st->rain_pool, ctx, character, true, true);
            break;
        case CB_RECLAIM_SPARK:
            particle_pool_reclaim(&st->spark_pool, ctx, character, true, true);
            break;
        default:
            break;
    }
}

static void thunderstorm_destroy(Effect *self) {
    Thunderstorm *st = self->state;
    if (!st) {
        return;
    }
    if (st->has_rain_pool) {
        particle_pool_free(&st->rain_pool);
    }
    if (st->has_spark_pool) {
        particle_pool_free(&st->spark_pool);
    }
    if (st->has_spark_gradient) {
        gradient_free(&st->spark_gradient);
    }
    free(st->pending_strike_chars);
    free(st->available_strike_chars);
    free(st->active_strike_chars);
    free(st->pending_glow_chars);
    free(st);
    free(self);
}

static const EffectOps THUNDERSTORM_OPS = {thunderstorm_build, thunderstorm_next_frame, thunderstorm_destroy,
                                           thunderstorm_callback};

Effect *thunderstorm_make(const void *cfg) {
    Thunderstorm *st = calloc(1, sizeof(Thunderstorm));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const ThunderstormConfig *)cfg;
    st->strike_branch_chance = 0.05;
    st->phase = PHASE_PRE_STORM;
    if (particle_pool_init(&st->rain_pool, (const char *const *)st->config.raindrop_symbols.items,
                           st->config.raindrop_symbols.len, false, 0, coord_new(0, 0)) != 0) {
        free(st);
        free(effect);
        return NULL;
    }
    st->has_rain_pool = true;
    if (particle_pool_init(&st->spark_pool, (const char *const *)st->config.spark_symbols.items,
                           st->config.spark_symbols.len, true, 2000, coord_new(0, 0)) != 0) {
        particle_pool_free(&st->rain_pool);
        free(st);
        free(effect);
        return NULL;
    }
    st->has_spark_pool = true;
    effect->ops = &THUNDERSTORM_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec thunderstorm_specs[] = {
    EF_SPEC("lightning-color", 0, EF_COLOR, offsetof(ThunderstormConfig, lightning_color)),
    EF_SPEC("glowing-text-color", 0, EF_COLOR, offsetof(ThunderstormConfig, glowing_text_color)),
    EF_SPEC("text-glow-time", 0, EF_POS_INT, offsetof(ThunderstormConfig, text_glow_time)),
    {"raindrop-symbols", 0, EF_STRING_LIST, offsetof(ThunderstormConfig, raindrop_symbols), NULL},
    {"spark-symbols", 0, EF_STRING_LIST, offsetof(ThunderstormConfig, spark_symbols), NULL},
    EF_SPEC("spark-glow-color", 0, EF_COLOR, offsetof(ThunderstormConfig, spark_glow_color)),
    EF_SPEC("spark-glow-time", 0, EF_POS_INT, offsetof(ThunderstormConfig, spark_glow_time)),
    EF_SPEC("storm-time", 0, EF_POS_INT, offsetof(ThunderstormConfig, storm_time)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(ThunderstormConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(ThunderstormConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-frames", 0, EF_POS_INT, offsetof(ThunderstormConfig, final_gradient_frames)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(ThunderstormConfig, final_gradient_direction)),
};

const EffectEntry thunderstorm_entry = {
    "thunderstorm",
    thunderstorm_specs,
    sizeof(thunderstorm_specs) / sizeof(thunderstorm_specs[0]),
    sizeof(ThunderstormConfig),
    thunderstorm_config_defaults,
    thunderstorm_free_config,
    thunderstorm_make,
};
