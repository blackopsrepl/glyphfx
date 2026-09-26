// laseretch, ported from the reference effects/laseretch.rs.
//
// Upstream quirk (effect_laseretch.py:404): the grouped-etch branch is dead
// code: --etch-pattern parses a CharacterGroup member but build() tests
// membership against the enum's NAME strings, which never matches. Only
// "algorithm" (the default) etches; grouped patterns leave pending_chars empty.
#include "effects/laseretch.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/motion.h"
#include "engine/particles.h"
#include "engine/terminal.h"
#include "utils/easing.h"
#include "utils/graphics.h"
#include "utils/ordmap.h"
#include "utils/spanning_tree.h"

/// Callback id: sparks_pool.reclaim(spark, hide=True, deactivate=True).
#define CB_RECLAIM_SPARK 0u

typedef struct {
    Coord position;
    CharId *beam_chars;
    size_t beam_chars_len;
    size_t beam_chars_cap;
    Gradient spark_gradient;
    ParticlePool sparks_pool;
    bool has_sparks_pool;
} Laser;

typedef struct {
    LaserEtchConfig config;
    ColorPair *character_final_color_map;
    bool *final_present;
    size_t map_len;
    CharId *pending_chars;
    size_t pending_len;
    size_t pending_cap;
    size_t pending_head;
    int64_t char_delay;
    Laser *laser;
} LaserEtch;

typedef struct {
    const Color *colors;
    size_t len;
    int64_t cooling_frames;
} SparkInit;

typedef struct {
    Effect *self;
    Coord position;
} SparkEmit;

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

static int parse_etch_pattern(const char *value, void *dst) {
    EtchPattern *out = dst;
    if (strcmp(value, "algorithm") == 0) {
        *out = ETCH_PATTERN_ALGORITHM;
        return 0;
    }
    static const char *groups[10] = {
        "column_left_to_right",          "column_right_to_left",
        "row_top_to_bottom",             "row_bottom_to_top",
        "diagonal_top_left_to_bottom_right", "diagonal_bottom_left_to_top_right",
        "diagonal_top_right_to_bottom_left", "diagonal_bottom_right_to_top_left",
        "center_to_outside",             "outside_to_center",
    };
    for (int i = 0; i < 10; i++) {
        if (strcmp(value, groups[i]) == 0) {
            *out = ETCH_PATTERN_GROUP;
            return 0;
        }
    }
    return -1;
}

void laseretch_config_defaults(void *cfg_ptr) {
    LaserEtchConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->etch_pattern = ETCH_PATTERN_ALGORITHM;
    cfg->etch_speed = 1;
    cfg->etch_delay = 1;
    push_color_default(&cfg->cool_gradient_stops, "ffe680");
    push_color_default(&cfg->cool_gradient_stops, "ff7b00");
    push_color_default(&cfg->laser_gradient_stops, "ffffff");
    push_color_default(&cfg->laser_gradient_stops, "376cff");
    push_color_default(&cfg->spark_gradient_stops, "ffffff");
    push_color_default(&cfg->spark_gradient_stops, "ffe680");
    push_color_default(&cfg->spark_gradient_stops, "ff7b00");
    push_color_default(&cfg->spark_gradient_stops, "1a0900");
    cfg->spark_cooling_frames = 7;
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "ffffff");
    push_int_default(&cfg->final_gradient_steps, 8);
    cfg->final_gradient_frames = 4;
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void laseretch_free_config(void *cfg_ptr) {
    LaserEtchConfig *cfg = cfg_ptr;
    free(cfg->cool_gradient_stops.items);
    free(cfg->laser_gradient_stops.items);
    free(cfg->spark_gradient_stops.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

/// Laser._make_sparks_pool initialize_sparks closure.
static void laseretch_initialize_spark(void *user, EngineCtx *ctx, CharId spark) {
    SparkInit *init = user;
    EffectCharacter *ch = &ctx->terminal.arena.items[spark];
    ch->layer = 2;
    size_t sym_len = strlen(ch->input_symbol);
    char *input_symbol = malloc(sym_len + 1);
    strcpy(input_symbol, ch->input_symbol);
    bool uses_pre = ch->uses_input_preexisting_colors;
    Easing ease;
    memset(&ease, 0, sizeof(ease));
    ch = &ctx->terminal.arena.items[spark];
    const char *spark_scn = animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, ease, "spark",
                                                uses_pre);
    Scene *scene = spark_scn ? (Scene *)om_get(&ctx->terminal.arena.items[spark].animation.scenes, spark_scn) : NULL;
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

/// LaserEtchIterator._has_input_colors.
static bool laseretch_has_input_colors(EngineCtx *ctx, CharId id) {
    Animation *anim = &ctx->terminal.arena.items[id].animation;
    return anim->has_input_fg || anim->has_input_bg;
}

/// emit's setup_spark_path closure.
static void laseretch_setup_spark(void *user, EngineCtx *ctx, CharId spark) {
    SparkEmit *emit = user;
    motion_set_coordinate(&ctx->terminal.arena.items[spark].motion, emit->position);
    Easing ease = easing_named(EASE_OUT_SINE);
    char *spark_path = NULL;
    if (motion_new_path(&ctx->terminal.arena.items[spark].motion, 0.3, true, ease, false, 0, 0, false, "",
                        &spark_path) != 0) {
        return;
    }
    int64_t fall_column = rng_randint(&ctx->rng, emit->position.column - 20, emit->position.column + 20);
    Coord fall_target_coord = coord_new(fall_column, ctx->terminal.canvas.bottom);
    Coord control = coord_new(fall_target_coord.column,
                              emit->position.row + rng_randint(&ctx->rng, -10, 20));
    Path *p = (Path *)om_get(&ctx->terminal.arena.items[spark].motion.paths, spark_path);
    Waypoint wp;
    memset(&wp, 0, sizeof(wp));
    (void)path_new_waypoint(p, fall_target_coord, &control, 1, "", &wp);
    waypoint_free(&wp);
    engine_activate_path(ctx, emit->self, spark, spark_path);
    engine_activate_scene(ctx, emit->self, spark, "spark");
    free(spark_path);
}

/// Laser.emit_sparks (+ its setup_spark_path closure).
static void laseretch_emit_sparks(Effect *self, EngineCtx *ctx, Laser *laser, size_t spark_count) {
    LaserEtch *st = self->state;
    SparkInit init;
    init.colors = laser->spark_gradient.spectrum;
    init.len = laser->spark_gradient.len;
    init.cooling_frames = st->config.spark_cooling_frames;
    for (size_t i = 0; i < spark_count; i++) {
        SparkEmit emit;
        emit.self = self;
        emit.position = laser->position;
        ParticleReset reset = particle_reset_default();
        particle_pool_emit(&laser->sparks_pool, ctx, laser->position, NULL, true, reset,
                           laseretch_initialize_spark, &init, laseretch_setup_spark, &emit);
    }
}

/// Laser.reposition.
static void laseretch_reposition(Effect *self, EngineCtx *ctx, Coord target) {
    LaserEtch *st = self->state;
    Laser *laser = st->laser;
    if (!laser) {
        return;
    }
    laser->position = target;
    int64_t row = target.row;
    int64_t col = target.column;
    for (size_t i = 0; i < laser->beam_chars_len; i++) {
        CharId char_id = laser->beam_chars[i];
        motion_set_coordinate(&ctx->terminal.arena.items[char_id].motion, coord_new(col, row));
        row += 1;
        col += 1;
    }
    laseretch_emit_sparks(self, ctx, laser, 1);
}

/// Laser.disable.
static void laseretch_disable(EngineCtx *ctx, Laser *laser) {
    for (size_t i = 0; i < laser->beam_chars_len; i++) {
        terminal_set_character_visibility(&ctx->terminal, laser->beam_chars[i], false);
    }
}

/// Laser.__init__ (+ _make_sparks_pool). The pool is created BEFORE the beam
/// characters, matching upstream's character_id allocation order.
static Laser *laseretch_make_laser(Effect *self, EngineCtx *ctx) {
    LaserEtch *st = self->state;
    Laser *laser = calloc(1, sizeof(Laser));
    if (!laser) {
        return NULL;
    }
    int64_t laser_steps[1] = {6};
    Gradient laser_gradient;
    if (gradient_new(st->config.laser_gradient_stops.items, st->config.laser_gradient_stops.len, laser_steps, 1,
                     true, true, &laser_gradient) != 0) {
        free(laser);
        return NULL;
    }
    int64_t spark_steps[2] = {3, 8};
    if (gradient_new(st->config.spark_gradient_stops.items, st->config.spark_gradient_stops.len, spark_steps, 2,
                     false, false, &laser->spark_gradient) != 0) {
        gradient_free(&laser_gradient);
        free(laser);
        return NULL;
    }

    static const char *spark_symbols[3] = {".", ",", "*"};
    if (particle_pool_init(&laser->sparks_pool, spark_symbols, 3, false, 0, coord_new(0, 0)) != 0) {
        gradient_free(&laser->spark_gradient);
        gradient_free(&laser_gradient);
        free(laser);
        return NULL;
    }
    laser->has_sparks_pool = true;
    SparkInit init;
    init.colors = laser->spark_gradient.spectrum;
    init.len = laser->spark_gradient.len;
    init.cooling_frames = st->config.spark_cooling_frames;
    if (particle_pool_preallocate(&laser->sparks_pool, ctx, 2000, laseretch_initialize_spark, &init) != 0) {
        particle_pool_free(&laser->sparks_pool);
        gradient_free(&laser->spark_gradient);
        gradient_free(&laser_gradient);
        free(laser);
        return NULL;
    }
    for (size_t i = 0; i < laser->sparks_pool.particles_len; i++) {
        CharId spark = laser->sparks_pool.particles[i];
        CallerKey caller;
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_SCENE;
        caller.id = "spark";
        EventAction action;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_CALLBACK;
        action.cb.id = CB_RECLAIM_SPARK;
        (void)engine_register_event(ctx, spark, EVENT_SCENE_COMPLETE, &caller, &action);
    }

    // beam characters up the diagonal from (0, 0)
    int64_t row = 0;
    int64_t col = 0;
    size_t rot = 0;
    size_t lg_len = laser_gradient.len;
    while (row <= ctx->terminal.canvas.top) {
        const char *symbol = laser->beam_chars_len == 0 ? "*" : "/";
        CharId char_id = terminal_add_character(&ctx->terminal, symbol, coord_new(col, row));
        ctx->terminal.arena.items[char_id].layer = 2;
        terminal_set_character_visibility(&ctx->terminal, char_id, true);
        row += 1;
        col += 1;
        idvec_push(&laser->beam_chars, &laser->beam_chars_len, &laser->beam_chars_cap, char_id);
        EffectCharacter *ch = &ctx->terminal.arena.items[char_id];
        size_t sym_len = strlen(ch->input_symbol);
        char *input_symbol = malloc(sym_len + 1);
        strcpy(input_symbol, ch->input_symbol);
        bool uses_pre = ch->uses_input_preexisting_colors;
        Easing ease;
        memset(&ease, 0, sizeof(ease));
        const char *laser_scn = animation_new_scene(&ctx->terminal.arena.items[char_id].animation, true, false,
                                                    SYNC_DISTANCE, false, ease, "laser", uses_pre);
        Scene *scene =
            laser_scn ? (Scene *)om_get(&ctx->terminal.arena.items[char_id].animation.scenes, laser_scn) : NULL;
        if (scene && lg_len > 0) {
            for (size_t i = 0; i < lg_len; i++) {
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                vp.colors.has_fg = true;
                vp.colors.fg = laser_gradient.spectrum[(rot + i) % lg_len];
                scene_add_frame(scene, input_symbol, 3, &vp);
            }
        }
        free(input_symbol);
        if (lg_len > 0) {
            rot = (rot + 1) % lg_len;
        }
        engine_activate_scene(ctx, self, char_id, "laser");
    }
    laser->position = coord_new(0, 0);
    gradient_free(&laser_gradient);
    return laser;
}

static int laseretch_build(Effect *self, EngineCtx *ctx) {
    LaserEtch *st = self->state;
    LaserEtchConfig *cfg = &st->config;

    // LaserEtchIterator.build
    Gradient final_fg_gradient;
    if (gradient_new(cfg->final_gradient_stops.items, cfg->final_gradient_stops.len,
                     cfg->final_gradient_steps.items, cfg->final_gradient_steps.len, false, false,
                     &final_fg_gradient) != 0) {
        return -1;
    }
    CoordColorMap final_gradient_mapping;
    if (gradient_build_coordinate_color_mapping(&final_fg_gradient, ctx->terminal.canvas.text_bottom,
                                                ctx->terminal.canvas.text_top, ctx->terminal.canvas.text_left,
                                                ctx->terminal.canvas.text_right,
                                                cfg->final_gradient_direction, &final_gradient_mapping) != 0) {
        gradient_free(&final_fg_gradient);
        return -1;
    }
    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    CharacterFilter filter = character_filter_default();
    size_t chars_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, filter,
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &chars_len);
    st->map_len = ctx->terminal.arena.len;
    st->character_final_color_map = calloc(st->map_len ? st->map_len : 1, sizeof(ColorPair));
    st->final_present = calloc(st->map_len ? st->map_len : 1, sizeof(bool));

    Easing ease;
    memset(&ease, 0, sizeof(ease));
    Color white;
    color_from_hex("ffffff", &white);
    Color spawn_color;
    color_from_hex("ffe680", &spawn_color);

    int rc = 0;
    for (size_t i = 0; i < chars_len && rc == 0; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        Coord input_coord = ch->input_coord;
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);
        bool has_input_fg = ch->animation.has_input_fg;
        Color input_fg = ch->animation.input_fg_color;
        bool has_input_bg = ch->animation.has_input_bg;
        Color input_bg = ch->animation.input_bg_color;
        bool uses_pre = ch->uses_input_preexisting_colors;

        ColorPair pair;
        memset(&pair, 0, sizeof(pair));
        Gradient cool_gradient;
        bool has_final_fg;
        bool has_final_bg;
        Color final_fg_color;
        Color final_bg_color;
        memset(&final_fg_color, 0, sizeof(final_fg_color));
        memset(&final_bg_color, 0, sizeof(final_bg_color));
        if (dynamic) {
            pair.has_fg = has_input_fg;
            pair.fg = input_fg;
            pair.has_bg = has_input_bg;
            pair.bg = input_bg;
            if (gradient_with_steps(cfg->cool_gradient_stops.items, cfg->cool_gradient_stops.len, 8, false,
                                    &cool_gradient) != 0) {
                rc = -1;
                free(input_symbol);
                break;
            }
            has_final_fg = pair.has_fg;
            final_fg_color = pair.fg;
            has_final_bg = pair.has_bg;
            final_bg_color = pair.bg;
        } else {
            const Color *mapped = coordcolormap_get(&final_gradient_mapping, input_coord);
            pair.has_fg = true;
            pair.fg = mapped ? *mapped : white;
            size_t stops_len = cfg->cool_gradient_stops.len + 1;
            Color *stops = malloc((stops_len ? stops_len : 1) * sizeof(Color));
            size_t si = 0;
            for (size_t j = 0; j < cfg->cool_gradient_stops.len; j++) {
                stops[si++] = cfg->cool_gradient_stops.items[j];
            }
            stops[si++] = pair.fg;
            int g_rc = gradient_with_steps(stops, stops_len, 8, false, &cool_gradient);
            free(stops);
            if (g_rc != 0) {
                rc = -1;
                free(input_symbol);
                break;
            }
            has_final_fg = pair.has_fg;
            final_fg_color = pair.fg;
            has_final_bg = pair.has_bg;
            final_bg_color = pair.bg;
        }
        if ((size_t)id < st->map_len) {
            st->character_final_color_map[id] = pair;
            st->final_present[id] = true;
        }

        Color cool_last = cool_gradient.spectrum[cool_gradient.len - 1];
        Gradient fg_gradient;
        Gradient bg_gradient;
        Gradient white_cooldown;
        bool has_fg_gradient = false;
        bool has_bg_gradient = false;
        bool has_white_cooldown = false;
        if (dynamic) {
            if (has_final_fg || has_final_bg) {
                if (has_final_fg) {
                    Color fstops[2] = {cool_last, final_fg_color};
                    if (gradient_with_steps(fstops, 2, 8, false, &fg_gradient) == 0) {
                        has_fg_gradient = true;
                    } else {
                        rc = -1;
                    }
                }
                if (rc == 0 && has_final_bg) {
                    Color bstops[2] = {cool_last, final_bg_color};
                    if (gradient_with_steps(bstops, 2, 8, false, &bg_gradient) == 0) {
                        has_bg_gradient = true;
                    } else {
                        rc = -1;
                    }
                }
            } else {
                Color wstops[2] = {cool_last, white};
                if (gradient_with_steps(wstops, 2, 8, false, &white_cooldown) == 0) {
                    has_white_cooldown = true;
                } else {
                    rc = -1;
                }
            }
        }
        if (rc == 0) {
            const char *spawn_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                        SYNC_DISTANCE, false, ease, "spawn", uses_pre);
            Scene *scene =
                spawn_scn ? (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, spawn_scn) : NULL;
            VisualParams hv;
            memset(&hv, 0, sizeof(hv));
            hv.has_colors = true;
            hv.colors.has_fg = true;
            hv.colors.fg = spawn_color;
            if (!scene || scene_add_frame(scene, "^", 3, &hv) != 0) {
                rc = -1;
            }
            for (size_t j = 0; j < cool_gradient.len && rc == 0; j++) {
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                vp.colors.has_fg = true;
                vp.colors.fg = cool_gradient.spectrum[j];
                if (scene_add_frame(scene, input_symbol, 3, &vp) != 0) {
                    rc = -1;
                }
            }
            if (rc == 0 && dynamic) {
                const char *syms[1] = {input_symbol};
                if (has_final_fg || has_final_bg) {
                    if (scene_apply_gradient_to_symbols(scene, syms, 1, 3,
                                                        has_fg_gradient ? &fg_gradient : NULL,
                                                        has_bg_gradient ? &bg_gradient : NULL) != 0) {
                        rc = -1;
                    }
                } else {
                    if (scene_apply_gradient_to_symbols(scene, syms, 1, 3,
                                                        has_white_cooldown ? &white_cooldown : NULL, NULL) != 0) {
                        rc = -1;
                    }
                    VisualParams dvp;
                    memset(&dvp, 0, sizeof(dvp));
                    dvp.has_colors = true;
                    if (rc == 0 && scene_add_frame(scene, input_symbol, 3, &dvp) != 0) {
                        rc = -1;
                    }
                }
            }
        }
        if (has_fg_gradient) {
            gradient_free(&fg_gradient);
        }
        if (has_bg_gradient) {
            gradient_free(&bg_gradient);
        }
        if (has_white_cooldown) {
            gradient_free(&white_cooldown);
        }
        gradient_free(&cool_gradient);

        if (rc == 0) {
            engine_activate_scene(ctx, self, id, "spawn");
        }
        free(input_symbol);
    }
    free(characters);
    coordcolormap_free(&final_gradient_mapping);
    gradient_free(&final_fg_gradient);
    if (rc != 0) {
        return -1;
    }

    if (cfg->etch_pattern == ETCH_PATTERN_ALGORITHM) {
        RecursiveBacktracker algo;
        if (recursive_backtracker_new(&algo, ctx, false, CHAR_ID_NONE, true) != 0) {
            return -1;
        }
        while (!algo.complete) {
            recursive_backtracker_step(&algo, ctx);
        }
        st->pending_chars = algo.char_link_order;
        st->pending_len = algo.char_link_order_len;
        st->pending_head = 0;
        st->pending_cap = algo.char_link_order_len;
        algo.char_link_order = NULL;
        recursive_backtracker_free(&algo);
    } else {
        st->pending_chars = NULL;
        st->pending_len = 0;
        st->pending_head = 0;
        st->pending_cap = 0;
    }

    // LaserEtchIterator.__init__ tail
    st->char_delay = 0;
    Laser *laser = laseretch_make_laser(self, ctx);
    if (!laser) {
        return -1;
    }
    for (size_t i = 0; i < laser->beam_chars_len; i++) {
        ac_insert(&ctx->active_characters, laser->beam_chars[i]);
    }
    st->laser = laser;
    return 0;
}

static const char *laseretch_next_frame(Effect *self, EngineCtx *ctx) {
    LaserEtch *st = self->state;
    bool has_pending = st->pending_head < st->pending_len;
    if (!has_pending && ac_is_empty(&ctx->active_characters)) {
        return NULL;
    }
    if (st->char_delay == 0) {
        for (int64_t i = 0; i < st->config.etch_speed; i++) {
            if (st->pending_head >= st->pending_len) {
                break;
            }
            CharId next_char = st->pending_chars[st->pending_head++];
            while (strcmp(ctx->terminal.arena.items[next_char].input_symbol, " ") == 0 &&
                   !laseretch_has_input_colors(ctx, next_char)) {
                if (st->pending_head < st->pending_len) {
                    next_char = st->pending_chars[st->pending_head++];
                } else {
                    break;
                }
            }
            terminal_set_character_visibility(&ctx->terminal, next_char, true);
            ac_insert(&ctx->active_characters, next_char);
            Coord target = ctx->terminal.arena.items[next_char].input_coord;
            laseretch_reposition(self, ctx, target);
        }
        st->char_delay = st->config.etch_delay;
    } else {
        st->char_delay -= 1;
    }
    if (st->pending_head < st->pending_len) {
        if (st->laser) {
            for (size_t i = 0; i < st->laser->beam_chars_len; i++) {
                ac_insert(&ctx->active_characters, st->laser->beam_chars[i]);
            }
        }
    } else {
        if (st->laser) {
            laseretch_disable(ctx, st->laser);
        }
    }
    engine_update(ctx, self);
    return engine_frame(ctx);
}

static void laseretch_callback(Effect *self, EngineCtx *ctx, CharId character, const EffectCallback *cb) {
    LaserEtch *st = self->state;
    if (cb->id == CB_RECLAIM_SPARK) {
        if (st->laser) {
            particle_pool_reclaim(&st->laser->sparks_pool, ctx, character, true, true);
        }
    }
}

static void laseretch_destroy(Effect *self) {
    LaserEtch *st = self->state;
    if (!st) {
        return;
    }
    if (st->laser) {
        if (st->laser->has_sparks_pool) {
            particle_pool_free(&st->laser->sparks_pool);
        }
        gradient_free(&st->laser->spark_gradient);
        free(st->laser->beam_chars);
        free(st->laser);
    }
    free(st->character_final_color_map);
    free(st->final_present);
    free(st->pending_chars);
    free(st);
    free(self);
}

static const EffectOps LASERETCH_OPS = {laseretch_build, laseretch_next_frame, laseretch_destroy,
                                         laseretch_callback};

Effect *laseretch_make(const void *cfg) {
    LaserEtch *st = calloc(1, sizeof(LaserEtch));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const LaserEtchConfig *)cfg;
    effect->ops = &LASERETCH_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec laseretch_specs[] = {
    {"etch-pattern", 0, EF_CUSTOM, offsetof(LaserEtchConfig, etch_pattern), parse_etch_pattern},
    EF_SPEC("etch-speed", 0, EF_POS_INT, offsetof(LaserEtchConfig, etch_speed)),
    EF_SPEC("etch-delay", 0, EF_NONNEG_INT, offsetof(LaserEtchConfig, etch_delay)),
    EF_SPEC("cool-gradient-stops", 0, EF_COLOR_LIST, offsetof(LaserEtchConfig, cool_gradient_stops)),
    EF_SPEC("laser-gradient-stops", 0, EF_COLOR_LIST, offsetof(LaserEtchConfig, laser_gradient_stops)),
    EF_SPEC("spark-gradient-stops", 0, EF_COLOR_LIST, offsetof(LaserEtchConfig, spark_gradient_stops)),
    EF_SPEC("spark-cooling-frames", 0, EF_POS_INT, offsetof(LaserEtchConfig, spark_cooling_frames)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(LaserEtchConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(LaserEtchConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-frames", 0, EF_POS_INT, offsetof(LaserEtchConfig, final_gradient_frames)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(LaserEtchConfig, final_gradient_direction)),
};

const EffectEntry laseretch_entry = {
    "laseretch",
    laseretch_specs,
    sizeof(laseretch_specs) / sizeof(laseretch_specs[0]),
    sizeof(LaserEtchConfig),
    laseretch_config_defaults,
    laseretch_free_config,
    laseretch_make,
};
