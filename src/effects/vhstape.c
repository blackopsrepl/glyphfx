// vhstape, ported from the reference effects/vhstape.rs.
//
// The inner VHSTapeIterator.Line class is the `VhsLine` struct; lines live in
// an array indexed the way upstream's `lines` dict keys 0..n-1 order them. The
// wave/glitch line lists hold indices (upstream holds Line objects compared by
// identity). The glitch budget counts frames, not clock reads.
#include "effects/vhstape.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/motion.h"
#include "engine/terminal.h"
#include "utils/easing.h"
#include "utils/graphics.h"
#include "utils/ordmap.h"
#include "utils/pycompat.h"

typedef struct {
    CharId *characters;
    size_t len;
} VhsLine;

typedef enum {
    VHS_GLITCHING,
    VHS_NOISE,
    VHS_REDRAW,
    VHS_COMPLETE,
} VhsPhase;

typedef struct {
    VhsTapeConfig config;
    VhsLine *lines;
    size_t lines_len;
    size_t lines_cap;
    bool has_active_glitch_wave_top;
    int64_t active_glitch_wave_top;
    size_t *active_glitch_wave_lines;
    size_t active_glitch_wave_len;
    size_t active_glitch_wave_cap;
    size_t *active_glitch_lines;
    size_t active_glitch_len;
    size_t active_glitch_cap;
    ColorPair *character_stable_color_map;
    ColorPair *character_final_color_map;
    size_t map_len;
    int64_t glitching_steps_elapsed;
    VhsPhase phase;
    size_t *to_redraw;
    size_t to_redraw_len;
    bool redrawing;
} VhsTape;

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

static void idxvec_push(size_t **items, size_t *len, size_t *cap, size_t v) {
    if (*len == *cap) {
        size_t c = *cap ? *cap * 2 : 8;
        size_t *grown = realloc(*items, c * sizeof(size_t));
        if (!grown) {
            return;
        }
        *items = grown;
        *cap = c;
    }
    (*items)[(*len)++] = v;
}

static bool idxvec_contains(const size_t *items, size_t len, size_t v) {
    for (size_t i = 0; i < len; i++) {
        if (items[i] == v) {
            return true;
        }
    }
    return false;
}

static Easing no_ease(void) {
    Easing e;
    memset(&e, 0, sizeof(e));
    return e;
}

void vhstape_config_defaults(void *cfg_ptr) {
    VhsTapeConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    push_color_default(&cfg->glitch_line_colors, "ffffff");
    push_color_default(&cfg->glitch_line_colors, "ff0000");
    push_color_default(&cfg->glitch_line_colors, "00ff00");
    push_color_default(&cfg->glitch_line_colors, "0000ff");
    push_color_default(&cfg->glitch_line_colors, "ffffff");
    push_color_default(&cfg->glitch_wave_colors, "ffffff");
    push_color_default(&cfg->glitch_wave_colors, "ff0000");
    push_color_default(&cfg->glitch_wave_colors, "00ff00");
    push_color_default(&cfg->glitch_wave_colors, "0000ff");
    push_color_default(&cfg->glitch_wave_colors, "ffffff");
    push_color_default(&cfg->noise_colors, "1e1e1f");
    push_color_default(&cfg->noise_colors, "3c3b3d");
    push_color_default(&cfg->noise_colors, "6d6c70");
    push_color_default(&cfg->noise_colors, "a2a1a6");
    push_color_default(&cfg->noise_colors, "cbc9cf");
    push_color_default(&cfg->noise_colors, "ffffff");
    cfg->glitch_line_chance = 0.05;
    cfg->noise_chance = 0.004;
    cfg->total_glitch_time = 600;
    push_color_default(&cfg->final_gradient_stops, "ab48ff");
    push_color_default(&cfg->final_gradient_stops, "e7b2b2");
    push_color_default(&cfg->final_gradient_stops, "fffebd");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void vhstape_free_config(void *cfg_ptr) {
    VhsTapeConfig *cfg = cfg_ptr;
    free(cfg->glitch_line_colors.items);
    free(cfg->glitch_wave_colors.items);
    free(cfg->noise_colors.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

/// Line.line_movement_complete.
static bool vhs_line_movement_complete(VhsTape *st, EngineCtx *ctx, size_t idx) {
    VhsLine *line = &st->lines[idx];
    for (size_t i = 0; i < line->len; i++) {
        if (!motion_movement_is_complete(&ctx->terminal.arena.items[line->characters[i]].motion)) {
            return false;
        }
    }
    return true;
}

/// VhsTape.insert_line_characters.
static void vhs_insert_line_characters(VhsTape *st, EngineCtx *ctx, size_t idx) {
    VhsLine *line = &st->lines[idx];
    for (size_t i = 0; i < line->len; i++) {
        ac_insert(&ctx->active_characters, line->characters[i]);
    }
}

/// Line.snow.
static void vhs_line_snow(Effect *self, EngineCtx *ctx, size_t idx) {
    VhsTape *st = self->state;
    VhsLine *line = &st->lines[idx];
    for (size_t i = 0; i < line->len; i++) {
        engine_activate_scene(ctx, self, line->characters[i], "snow");
    }
}

/// Line.set_hold_time.
static void vhs_line_set_hold_time(VhsTape *st, EngineCtx *ctx, size_t idx, int64_t hold_time) {
    VhsLine *line = &st->lines[idx];
    for (size_t i = 0; i < line->len; i++) {
        CharId id = line->characters[i];
        Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, "glitch");
        if (p) {
            p->hold_time = hold_time;
        }
    }
}

/// Line.activate_path.
static void vhs_line_activate_path(Effect *self, EngineCtx *ctx, size_t idx, const char *path_id) {
    VhsTape *st = self->state;
    VhsLine *line = &st->lines[idx];
    for (size_t i = 0; i < line->len; i++) {
        engine_activate_path(ctx, self, line->characters[i], path_id);
    }
}

/// Line.glitch.
static void vhs_line_glitch(Effect *self, EngineCtx *ctx, size_t idx, bool final_) {
    VhsTape *st = self->state;
    VhsLine *line = &st->lines[idx];
    for (size_t i = 0; i < line->len; i++) {
        CharId id = line->characters[i];
        if (final_) {
            Path *gp = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, "glitch");
            if (gp) {
                gp->hold_time = 0;
            }
            Path *rp = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, "restore");
            if (rp) {
                rp->hold_time = 0;
            }
        }
        double glitch_speed = 40.0 / (double)rng_randint(&ctx->rng, 20, 40);
        double restore_speed = 40.0 / (double)rng_randint(&ctx->rng, 20, 40);
        Path *gp = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, "glitch");
        if (gp) {
            gp->speed = glitch_speed;
        }
        Path *rp = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, "restore");
        if (rp) {
            rp->speed = restore_speed;
        }
        engine_activate_path(ctx, self, id, "glitch");
    }
}

/// Line.restore.
static void vhs_line_restore(Effect *self, EngineCtx *ctx, size_t idx) {
    VhsTape *st = self->state;
    VhsLine *line = &st->lines[idx];
    for (size_t i = 0; i < line->len; i++) {
        CharId id = line->characters[i];
        double restore_speed = 40.0 / (double)rng_randint(&ctx->rng, 20, 40);
        Path *rp = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, "restore");
        if (rp) {
            rp->speed = restore_speed;
        }
        engine_activate_path(ctx, self, id, "restore");
    }
}

/// Line.build_line_effects.
static int vhs_build_line_effects(Effect *self, EngineCtx *ctx, CharId *characters, size_t n) {
    VhsTape *st = self->state;
    VhsTapeConfig *cfg = &st->config;
    Color *glitch_line_colors = cfg->glitch_line_colors.items;
    size_t glitch_line_n = cfg->glitch_line_colors.len;
    Color *noise_colors = cfg->noise_colors.items;
    size_t noise_n = cfg->noise_colors.len;
    static const char *snow_chars[4] = {"#", "*", ".", ":"};

    int64_t offset = rng_randint(&ctx->rng, 4, 25);
    static const int64_t directions[2] = {-1, 1};
    int64_t direction = directions[rng_choice_index(&ctx->rng, 2)];
    int64_t hold_time = rng_randint(&ctx->rng, 1, 50);

    Easing ease = no_ease();

    for (size_t ci = 0; ci < n; ci++) {
        CharId id = characters[ci];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        Coord input_coord = ch->input_coord;
        ColorPair stable_colors = st->character_stable_color_map[id];
        ColorPair final_colors = st->character_final_color_map[id];

        // make glitch and restore waypoints + glitch wave waypoints
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, 2.0, false, ease, false, 0, hold_time, false,
                            "glitch", NULL) != 0) {
            return -1;
        }
        {
            Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, "glitch");
            Waypoint wp;
            memset(&wp, 0, sizeof(wp));
            (void)path_new_waypoint(p, coord_new(input_coord.column + offset * direction, input_coord.row), NULL, 0,
                                    "glitch", &wp);
            waypoint_free(&wp);
        }
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, 2.0, false, ease, false, 0, 0, false, "restore",
                            NULL) != 0) {
            return -1;
        }
        {
            Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, "restore");
            Waypoint wp;
            memset(&wp, 0, sizeof(wp));
            (void)path_new_waypoint(p, input_coord, NULL, 0, "restore", &wp);
            waypoint_free(&wp);
        }
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, 2.0, false, ease, false, 0, 0, false,
                            "glitch_wave_mid", NULL) != 0) {
            return -1;
        }
        {
            Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, "glitch_wave_mid");
            Waypoint wp;
            memset(&wp, 0, sizeof(wp));
            (void)path_new_waypoint(p, coord_new(input_coord.column + 8, input_coord.row), NULL, 0, "glitch_wave_mid",
                                    &wp);
            waypoint_free(&wp);
        }
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, 2.0, false, ease, false, 0, 0, false,
                            "glitch_wave_end", NULL) != 0) {
            return -1;
        }
        {
            Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, "glitch_wave_end");
            Waypoint wp;
            memset(&wp, 0, sizeof(wp));
            (void)path_new_waypoint(p, coord_new(input_coord.column + 14, input_coord.row), NULL, 0, "glitch_wave_end",
                                    &wp);
            waypoint_free(&wp);
        }

        // make glitch scenes
        bool uses_pre = ctx->terminal.arena.items[id].uses_input_preexisting_colors;
        const char *input_symbol = ctx->terminal.arena.items[id].input_symbol;
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors = stable_colors;

        animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false, SYNC_DISTANCE, false, ease,
                            "base", uses_pre);
        {
            Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "base");
            if (!scene || scene_add_frame(scene, input_symbol, 1, &vp) != 0) {
                return -1;
            }
        }
        animation_new_scene(&ctx->terminal.arena.items[id].animation, false, true, SYNC_STEP, false, ease,
                            "rgb_glitch_fwd", uses_pre);
        {
            Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "rgb_glitch_fwd");
            if (!scene) {
                return -1;
            }
            for (size_t k = 0; k < glitch_line_n; k++) {
                VisualParams fvp;
                memset(&fvp, 0, sizeof(fvp));
                fvp.has_colors = true;
                fvp.colors.has_fg = true;
                fvp.colors.fg = glitch_line_colors[k];
                if (scene_add_frame(scene, input_symbol, 1, &fvp) != 0) {
                    return -1;
                }
            }
        }
        animation_new_scene(&ctx->terminal.arena.items[id].animation, false, true, SYNC_STEP, false, ease,
                            "rgb_glitch_bwd", uses_pre);
        {
            Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "rgb_glitch_bwd");
            if (!scene) {
                return -1;
            }
            for (size_t k = glitch_line_n; k > 0; k--) {
                VisualParams fvp;
                memset(&fvp, 0, sizeof(fvp));
                fvp.has_colors = true;
                fvp.colors.has_fg = true;
                fvp.colors.fg = glitch_line_colors[k - 1];
                if (scene_add_frame(scene, input_symbol, 1, &fvp) != 0) {
                    return -1;
                }
            }
        }
        animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false, SYNC_DISTANCE, false, ease,
                            "snow", uses_pre);
        {
            Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "snow");
            if (!scene) {
                return -1;
            }
            for (int k = 0; k < 25; k++) {
                const char *symbol = snow_chars[rng_choice_index(&ctx->rng, 4)];
                Color color = noise_colors[rng_choice_index(&ctx->rng, noise_n)];
                VisualParams svp;
                memset(&svp, 0, sizeof(svp));
                svp.has_colors = true;
                svp.colors.has_fg = true;
                svp.colors.fg = color;
                if (scene_add_frame(scene, symbol, 2, &svp) != 0) {
                    return -1;
                }
            }
            if (scene_add_frame(scene, input_symbol, 1, &vp) != 0) {
                return -1;
            }
        }
        animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false, SYNC_DISTANCE, false, ease,
                            "final_snow", uses_pre);
        animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false, SYNC_DISTANCE, false, ease,
                            "final_redraw", uses_pre);
        {
            Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "final_redraw");
            if (!scene) {
                return -1;
            }
            VisualParams wvp;
            memset(&wvp, 0, sizeof(wvp));
            wvp.has_colors = true;
            wvp.colors.has_fg = true;
            color_from_hex("ffffff", &wvp.colors.fg);
            if (scene_add_frame(scene, "█", 6, &wvp) != 0) {
                return -1;
            }
            VisualParams fvp;
            memset(&fvp, 0, sizeof(fvp));
            fvp.has_colors = true;
            fvp.colors = final_colors;
            if (scene_add_frame(scene, input_symbol, 1, &fvp) != 0) {
                return -1;
            }
        }
        {
            Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "final_snow");
            if (!scene) {
                return -1;
            }
            for (int k = 0; k < 30; k++) {
                const char *symbol = snow_chars[rng_choice_index(&ctx->rng, 4)];
                Color color = noise_colors[rng_choice_index(&ctx->rng, noise_n)];
                VisualParams svp;
                memset(&svp, 0, sizeof(svp));
                svp.has_colors = true;
                svp.colors.has_fg = true;
                svp.colors.fg = color;
                if (scene_add_frame(scene, symbol, 2, &svp) != 0) {
                    return -1;
                }
            }
        }

        // register events
        CallerKey caller;
        EventAction action;
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = "glitch";
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_PATH;
        action.has_id = true;
        action.id = "restore";
        if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
            return -1;
        }
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = "glitch";
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_SCENE;
        action.has_id = true;
        action.id = "rgb_glitch_fwd";
        if (engine_register_event(ctx, id, EVENT_PATH_ACTIVATED, &caller, &action) != 0) {
            return -1;
        }
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = "restore";
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_SCENE;
        action.has_id = true;
        action.id = "rgb_glitch_bwd";
        if (engine_register_event(ctx, id, EVENT_PATH_ACTIVATED, &caller, &action) != 0) {
            return -1;
        }
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = "glitch_wave_mid";
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_SCENE;
        action.has_id = true;
        action.id = "rgb_glitch_fwd";
        if (engine_register_event(ctx, id, EVENT_PATH_ACTIVATED, &caller, &action) != 0) {
            return -1;
        }
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = "glitch_wave_end";
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_SCENE;
        action.has_id = true;
        action.id = "rgb_glitch_fwd";
        if (engine_register_event(ctx, id, EVENT_PATH_ACTIVATED, &caller, &action) != 0) {
            return -1;
        }
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_SCENE;
        caller.id = "rgb_glitch_bwd";
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_SCENE;
        action.has_id = true;
        action.id = "base";
        if (engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
            return -1;
        }
    }
    return 0;
}

/// VHSTapeIterator.glitch_wave.
static void vhs_glitch_wave(Effect *self, EngineCtx *ctx) {
    VhsTape *st = self->state;
    if (!st->has_active_glitch_wave_top || st->active_glitch_wave_top == 0) {
        if (ctx->terminal.canvas.text_height >= 3) {
            int64_t lower = 3;
            int64_t half = py_round_half_even((double)ctx->terminal.canvas.text_height * 0.5);
            if (half > lower) {
                lower = half;
            }
            int64_t pick =
                rng_randint(&ctx->rng, lower, ctx->terminal.canvas.text_height);
            st->active_glitch_wave_top = ctx->terminal.canvas.text_bottom + pick;
            st->has_active_glitch_wave_top = true;
        } else {
            return;
        }
    }

    bool all_complete = true;
    for (size_t i = 0; i < st->active_glitch_wave_len; i++) {
        if (!vhs_line_movement_complete(st, ctx, st->active_glitch_wave_lines[i])) {
            all_complete = false;
            break;
        }
    }
    if (!all_complete) {
        return;
    }

    if (st->active_glitch_wave_len > 0) {
        bool should_move = rng_random(&ctx->rng) < 0.3;
        int64_t wave_top_delta = 0;
        if (should_move) {
            wave_top_delta = rng_random(&ctx->rng) < 0.3 ? 1 : -1;
        }
        int64_t top = st->active_glitch_wave_top + wave_top_delta;
        int64_t clamped = top < ctx->terminal.canvas.text_top ? top : ctx->terminal.canvas.text_top;
        if (clamped < 2) {
            clamped = 2;
        }
        st->active_glitch_wave_top = clamped;
    }

    int64_t wave_top = st->active_glitch_wave_top;
    size_t *new_wave_lines = NULL;
    size_t new_wave_len = 0;
    size_t new_wave_cap = 0;
    for (int64_t line_index = wave_top - 2; line_index <= wave_top; line_index++) {
        int64_t adjusted_line_index = line_index - (ctx->terminal.canvas.text_bottom - 1);
        if (adjusted_line_index >= 0 && (size_t)adjusted_line_index < st->lines_len) {
            idxvec_push(&new_wave_lines, &new_wave_len, &new_wave_cap, (size_t)adjusted_line_index);
        }
    }

    size_t *old_wave_lines = st->active_glitch_wave_lines;
    size_t old_wave_len = st->active_glitch_wave_len;
    for (size_t i = 0; i < old_wave_len; i++) {
        size_t idx = old_wave_lines[i];
        if (!idxvec_contains(new_wave_lines, new_wave_len, idx)) {
            vhs_line_restore(self, ctx, idx);
            vhs_insert_line_characters(st, ctx, idx);
        }
    }
    free(old_wave_lines);
    st->active_glitch_wave_lines = new_wave_lines;
    st->active_glitch_wave_len = new_wave_len;
    st->active_glitch_wave_cap = new_wave_cap;

    if (wave_top < ctx->terminal.canvas.text_bottom + 2) {
        size_t *wave_lines = st->active_glitch_wave_lines;
        size_t wave_lines_len = st->active_glitch_wave_len;
        st->active_glitch_wave_lines = NULL;
        st->active_glitch_wave_len = 0;
        st->active_glitch_wave_cap = 0;
        for (size_t i = 0; i < wave_lines_len; i++) {
            vhs_line_restore(self, ctx, wave_lines[i]);
            vhs_insert_line_characters(st, ctx, wave_lines[i]);
        }
        free(wave_lines);
        st->has_active_glitch_wave_top = false;
        st->active_glitch_wave_top = 0;
    } else {
        static const char *path_ids[3] = {"glitch_wave_mid", "glitch_wave_end", "glitch_wave_mid"};
        size_t count = st->active_glitch_wave_len < 3 ? st->active_glitch_wave_len : 3;
        for (size_t i = 0; i < count; i++) {
            size_t idx = st->active_glitch_wave_lines[i];
            vhs_line_activate_path(self, ctx, idx, path_ids[i]);
            vhs_insert_line_characters(st, ctx, idx);
        }
    }
}

static int vhstape_build(Effect *self, EngineCtx *ctx) {
    VhsTape *st = self->state;
    VhsTapeConfig *cfg = &st->config;

    Gradient final_gradient;
    if (gradient_new(cfg->final_gradient_stops.items, cfg->final_gradient_stops.len,
                     cfg->final_gradient_steps.items, cfg->final_gradient_steps.len, false, false,
                     &final_gradient) != 0) {
        return -1;
    }
    CoordColorMap final_gradient_mapping;
    if (gradient_build_coordinate_color_mapping(&final_gradient, ctx->terminal.canvas.text_bottom,
                                                ctx->terminal.canvas.text_top, ctx->terminal.canvas.text_left,
                                                ctx->terminal.canvas.text_right,
                                                cfg->final_gradient_direction, &final_gradient_mapping) != 0) {
        gradient_free(&final_gradient);
        return -1;
    }

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    CharacterFilter filter = character_filter_default();
    size_t chars_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, filter,
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &chars_len);
    st->map_len = ctx->terminal.arena.len;
    st->character_stable_color_map = calloc(st->map_len ? st->map_len : 1, sizeof(ColorPair));
    st->character_final_color_map = calloc(st->map_len ? st->map_len : 1, sizeof(ColorPair));
    for (size_t i = 0; i < chars_len; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        if (dynamic) {
            bool has_fg = ch->animation.has_input_fg;
            Color input_fg = ch->animation.input_fg_color;
            bool has_bg = ch->animation.has_input_bg;
            Color input_bg = ch->animation.input_bg_color;
            ColorPair stable;
            memset(&stable, 0, sizeof(stable));
            stable.has_bg = has_bg;
            stable.bg = input_bg;
            stable.has_fg = true;
            if (has_fg) {
                stable.fg = input_fg;
            } else {
                color_from_hex("808080", &stable.fg);
            }
            ColorPair finalp;
            memset(&finalp, 0, sizeof(finalp));
            finalp.has_fg = has_fg;
            finalp.fg = input_fg;
            finalp.has_bg = has_bg;
            finalp.bg = input_bg;
            if ((size_t)id < st->map_len) {
                st->character_stable_color_map[id] = stable;
                st->character_final_color_map[id] = finalp;
            }
        } else {
            const Color *mapped = coordcolormap_get(&final_gradient_mapping, ch->input_coord);
            ColorPair stable;
            memset(&stable, 0, sizeof(stable));
            stable.has_fg = true;
            if (mapped) {
                stable.fg = *mapped;
            } else {
                color_from_hex("ffffff", &stable.fg);
            }
            if ((size_t)id < st->map_len) {
                st->character_stable_color_map[id] = stable;
                st->character_final_color_map[id] = stable;
            }
        }
    }
    free(characters);

    CharIdGrouping rows = terminal_get_characters_grouped(&ctx->terminal, character_filter_default(),
                                                          CG_ROW_BOTTOM_TO_TOP);
    for (size_t i = 0; i < rows.len; i++) {
        if (st->lines_len == st->lines_cap) {
            size_t cap = st->lines_cap ? st->lines_cap * 2 : 8;
            VhsLine *grown = realloc(st->lines, cap * sizeof(VhsLine));
            if (!grown) {
                free(rows.buckets);
                return -1;
            }
            st->lines = grown;
            st->lines_cap = cap;
        }
        VhsLine line;
        line.characters = rows.buckets[i].items;
        line.len = rows.buckets[i].len;
        if (vhs_build_line_effects(self, ctx, line.characters, line.len) != 0) {
            free(line.characters);
            free(rows.buckets);
            return -1;
        }
        st->lines[st->lines_len++] = line;
    }
    free(rows.buckets);

    characters = terminal_get_characters(&ctx->terminal, &ctx->rng, filter, CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT,
                                         &chars_len);
    for (size_t i = 0; i < chars_len; i++) {
        CharId id = characters[i];
        terminal_set_character_visibility(&ctx->terminal, id, true);
        engine_activate_scene(ctx, self, id, "base");
    }
    free(characters);

    st->glitching_steps_elapsed = 0;
    st->phase = VHS_GLITCHING;
    st->to_redraw = malloc((st->lines_len ? st->lines_len : 1) * sizeof(size_t));
    st->to_redraw_len = st->lines_len;
    for (size_t i = 0; i < st->lines_len; i++) {
        st->to_redraw[i] = i;
    }
    st->redrawing = false;

    coordcolormap_free(&final_gradient_mapping);
    gradient_free(&final_gradient);
    return 0;
}

static const char *vhstape_next_frame(Effect *self, EngineCtx *ctx) {
    VhsTape *st = self->state;
    if (st->phase == VHS_COMPLETE && ac_is_empty(&ctx->active_characters)) {
        return NULL;
    }
    switch (st->phase) {
        case VHS_GLITCHING: {
            bool all_complete = true;
            for (size_t i = 0; i < st->active_glitch_wave_len; i++) {
                if (!vhs_line_movement_complete(st, ctx, st->active_glitch_wave_lines[i])) {
                    all_complete = false;
                    break;
                }
            }
            if (st->active_glitch_wave_len == 0 || all_complete) {
                vhs_glitch_wave(self, ctx);
            }
            size_t keep = 0;
            for (size_t i = 0; i < st->active_glitch_len; i++) {
                size_t idx = st->active_glitch_lines[i];
                if (!vhs_line_movement_complete(st, ctx, idx)) {
                    st->active_glitch_lines[keep++] = idx;
                }
            }
            st->active_glitch_len = keep;

            if (rng_random(&ctx->rng) < st->config.glitch_line_chance && st->active_glitch_len < 3) {
                size_t glitch_line = (size_t)rng_choice_index(&ctx->rng, st->lines_len);
                if (!idxvec_contains(st->active_glitch_wave_lines, st->active_glitch_wave_len, glitch_line) &&
                    !idxvec_contains(st->active_glitch_lines, st->active_glitch_len, glitch_line)) {
                    int64_t hold_time = rng_randint(&ctx->rng, 20, 75);
                    vhs_line_set_hold_time(st, ctx, glitch_line, hold_time);
                    idxvec_push(&st->active_glitch_lines, &st->active_glitch_len, &st->active_glitch_cap,
                                glitch_line);
                    vhs_line_glitch(self, ctx, glitch_line, false);
                    vhs_insert_line_characters(st, ctx, glitch_line);
                }
            }

            if (rng_random(&ctx->rng) < st->config.noise_chance) {
                for (size_t idx = 0; idx < st->lines_len; idx++) {
                    vhs_line_snow(self, ctx, idx);
                    if (!idxvec_contains(st->active_glitch_wave_lines, st->active_glitch_wave_len, idx) &&
                        !idxvec_contains(st->active_glitch_lines, st->active_glitch_len, idx)) {
                        vhs_insert_line_characters(st, ctx, idx);
                    }
                }
            }

            st->glitching_steps_elapsed += 1;
            if (st->glitching_steps_elapsed >= st->config.total_glitch_time) {
                size_t *wave_lines = st->active_glitch_wave_lines;
                size_t wave_lines_len = st->active_glitch_wave_len;
                for (size_t i = 0; i < wave_lines_len; i++) {
                    vhs_line_restore(self, ctx, wave_lines[i]);
                }
                size_t *glitch_lines = st->active_glitch_lines;
                size_t glitch_lines_len = st->active_glitch_len;
                for (size_t i = 0; i < glitch_lines_len; i++) {
                    vhs_line_restore(self, ctx, glitch_lines[i]);
                }
                st->phase = VHS_NOISE;
            }
            break;
        }
        case VHS_NOISE:
            if (ac_is_empty(&ctx->active_characters)) {
                CharacterFilter filter = character_filter_default();
                size_t chars_len = 0;
                CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, filter,
                                                             CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &chars_len);
                for (size_t i = 0; i < chars_len; i++) {
                    CharId id = characters[i];
                    engine_activate_scene(ctx, self, id, "final_snow");
                    ac_insert(&ctx->active_characters, id);
                }
                free(characters);
                st->phase = VHS_REDRAW;
            }
            break;
        case VHS_REDRAW:
            if (st->redrawing || ac_is_empty(&ctx->active_characters)) {
                st->redrawing = true;
                if (st->to_redraw_len > 0) {
                    size_t next_line = st->to_redraw[--st->to_redraw_len];
                    VhsLine *line = &st->lines[next_line];
                    for (size_t i = 0; i < line->len; i++) {
                        CharId id = line->characters[i];
                        engine_activate_scene(ctx, self, id, "final_redraw");
                        ac_insert(&ctx->active_characters, id);
                    }
                } else {
                    st->phase = VHS_COMPLETE;
                }
            }
            break;
        case VHS_COMPLETE:
            break;
    }
    engine_update(ctx, self);
    return engine_frame(ctx);
}

static void vhstape_destroy(Effect *self) {
    VhsTape *st = self->state;
    if (!st) {
        return;
    }
    for (size_t i = 0; i < st->lines_len; i++) {
        free(st->lines[i].characters);
    }
    free(st->lines);
    free(st->active_glitch_wave_lines);
    free(st->active_glitch_lines);
    free(st->character_stable_color_map);
    free(st->character_final_color_map);
    free(st->to_redraw);
    free(st);
    free(self);
}

static const EffectOps VHSTAPE_OPS = {vhstape_build, vhstape_next_frame, vhstape_destroy, NULL};

Effect *vhstape_make(const void *cfg) {
    VhsTape *st = calloc(1, sizeof(VhsTape));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const VhsTapeConfig *)cfg;
    st->phase = VHS_GLITCHING;
    effect->ops = &VHSTAPE_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec vhstape_specs[] = {
    EF_SPEC("glitch-line-colors", 0, EF_COLOR_LIST, offsetof(VhsTapeConfig, glitch_line_colors)),
    EF_SPEC("glitch-wave-colors", 0, EF_COLOR_LIST, offsetof(VhsTapeConfig, glitch_wave_colors)),
    EF_SPEC("noise-colors", 0, EF_COLOR_LIST, offsetof(VhsTapeConfig, noise_colors)),
    EF_SPEC("glitch-line-chance", 0, EF_RATIO_NONNEG, offsetof(VhsTapeConfig, glitch_line_chance)),
    EF_SPEC("noise-chance", 0, EF_RATIO_NONNEG, offsetof(VhsTapeConfig, noise_chance)),
    EF_SPEC("total-glitch-time", 0, EF_POS_INT, offsetof(VhsTapeConfig, total_glitch_time)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(VhsTapeConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(VhsTapeConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(VhsTapeConfig, final_gradient_direction)),
};

const EffectEntry vhstape_entry = {
    "vhstape",
    vhstape_specs,
    sizeof(vhstape_specs) / sizeof(vhstape_specs[0]),
    sizeof(VhsTapeConfig),
    vhstape_config_defaults,
    vhstape_free_config,
    vhstape_make,
};
