#include "effects/fireworks.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/canvas.h"
#include "engine/character.h"
#include "engine/ctx.h"
#include "engine/motion.h"
#include "engine/terminal.h"
#include "utils/easing.h"
#include "utils/geometry.h"
#include "utils/graphics.h"
#include "utils/pycompat.h"

typedef struct {
    CharId *items;
    size_t len;
    size_t cap;
} IdVec;

typedef struct {
    FireworksConfig config;
    IdVec *shells;
    size_t shells_len;
    size_t shells_cap;
    int64_t firework_volume;
    int64_t explode_distance;
    ColorPair *character_final_color_map;
    bool *final_present;
    size_t map_len;
    int64_t launch_delay;
} Fireworks;

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

static void idvec_push(IdVec *v, CharId id) {
    if (v->len == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 16;
        CharId *grown = realloc(v->items, cap * sizeof(CharId));
        if (!grown) {
            return;
        }
        v->items = grown;
        v->cap = cap;
    }
    v->items[v->len++] = id;
}

static void idvec_free(IdVec *v) {
    free(v->items);
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

static void shells_push(Fireworks *st, IdVec *shell) {
    if (st->shells_len == st->shells_cap) {
        size_t cap = st->shells_cap ? st->shells_cap * 2 : 8;
        IdVec *grown = realloc(st->shells, cap * sizeof(IdVec));
        if (!grown) {
            return;
        }
        st->shells = grown;
        st->shells_cap = cap;
    }
    st->shells[st->shells_len++] = *shell;
    memset(shell, 0, sizeof(*shell));
}

void fireworks_config_defaults(void *cfg_ptr) {
    FireworksConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->explode_anywhere = false;
    const char *colors[5] = {"88F7E2", "44D492", "F5EB67", "FFA15C", "FA233E"};
    for (size_t i = 0; i < 5; i++) {
        push_color_default(&cfg->firework_colors, colors[i]);
    }
    cfg->firework_symbol = malloc(2);
    strcpy(cfg->firework_symbol, "o");
    cfg->firework_volume = 0.05;
    cfg->launch_delay = 45;
    cfg->explode_distance = 0.2;
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "FFFFFF");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_HORIZONTAL;
}

void fireworks_free_config(void *cfg_ptr) {
    FireworksConfig *cfg = cfg_ptr;
    free(cfg->firework_colors.items);
    free(cfg->firework_symbol);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int prepare_waypoints(Fireworks *st, Effect *self, EngineCtx *ctx) {
    FireworksConfig *cfg = &st->config;
    IdVec firework_shell;
    memset(&firework_shell, 0, sizeof(firework_shell));

    size_t chars_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &chars_len);
    int64_t canvas_bottom = ctx->terminal.canvas.bottom;
    int64_t canvas_top = ctx->terminal.canvas.top;
    int64_t canvas_right = ctx->terminal.canvas.right;

    int64_t origin_x = 0;
    Coord origin_coord = coord_new(0, 0);
    CoordVec explode_waypoint_coords;
    coordvec_init(&explode_waypoint_coords);

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    int rc = 0;
    for (size_t ci = 0; ci < chars_len && rc == 0; ci++) {
        CharId id = characters[ci];
        if ((int64_t)firework_shell.len == st->firework_volume || firework_shell.len == 0) {
            origin_x = rng_randrange(&ctx->rng, 0, canvas_right);
            shells_push(st, &firework_shell);
            int64_t min_row = !cfg->explode_anywhere ? ctx->terminal.arena.items[id].input_coord.row
                                                     : canvas_bottom;
            int64_t origin_y = rng_randrange(&ctx->rng, min_row, canvas_top + 1);
            origin_coord = coord_new(origin_x, origin_y);
            coordvec_free(&explode_waypoint_coords);
            explode_waypoint_coords = find_coords_in_circle(origin_coord, st->explode_distance);
        }
        Coord input_coord = ctx->terminal.arena.items[id].input_coord;

        motion_set_coordinate(&ctx->terminal.arena.items[id].motion, coord_new(origin_x, canvas_bottom));
        char *apex_path = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.35, true, easing_named(EASE_OUT_EXPO), true,
                            2, 0, false, "apex_pth", &apex_path) != 0) {
            rc = -1;
            break;
        }
        Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, apex_path);
        Waypoint wp;
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(p, origin_coord, NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(apex_path);
            rc = -1;
            break;
        }
        waypoint_free(&wp);
        Coord apex_wpt_coord = origin_coord;

        double explode_speed = rng_uniform(&ctx->rng, 0.2, 0.4);
        char *explode_path = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, explode_speed, true, easing_named(EASE_OUT_CIRC),
                            true, 2, 0, false, "", &explode_path) != 0) {
            free(apex_path);
            rc = -1;
            break;
        }
        Coord explode_wpt_coord =
            explode_waypoint_coords.items[rng_choice_index(&ctx->rng, explode_waypoint_coords.len)];
        p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, explode_path);
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(p, explode_wpt_coord, NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(explode_path);
            free(apex_path);
            rc = -1;
            break;
        }
        waypoint_free(&wp);

        Coord bloom_control_point =
            extrapolate_along_ray(apex_wpt_coord, explode_wpt_coord, (double)py_floor_div(st->explode_distance, 2));
        int64_t bloom_row = bloom_control_point.row - 7;
        if (bloom_row < 1) {
            bloom_row = 1;
        }
        Coord bloom_wpt_coord = coord_new(bloom_control_point.column, bloom_row);
        p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, explode_path);
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(p, bloom_wpt_coord, &bloom_control_point, 1, "", &wp) != 0) {
            waypoint_free(&wp);
            free(explode_path);
            free(apex_path);
            rc = -1;
            break;
        }
        waypoint_free(&wp);

        char *input_path = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.6, true, easing_named(EASE_IN_OUT_QUART), true,
                            2, 0, false, "input_pth", &input_path) != 0) {
            free(explode_path);
            free(apex_path);
            rc = -1;
            break;
        }
        Coord input_control_point = coord_new(bloom_wpt_coord.column, 1);
        p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, input_path);
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(p, input_coord, &input_control_point, 1, "", &wp) != 0) {
            waypoint_free(&wp);
            free(input_path);
            free(explode_path);
            free(apex_path);
            rc = -1;
            break;
        }
        waypoint_free(&wp);

        CallerKey caller;
        EventAction action;
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = "apex_pth";
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_PATH;
        action.has_id = true;
        action.id = explode_path;
        if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
            rc = -1;
        }
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = explode_path;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_PATH;
        action.has_id = true;
        action.id = "input_pth";
        if (rc == 0 && engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
            rc = -1;
        }
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = "input_pth";
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_SET_LAYER;
        action.layer = 0;
        if (rc == 0 && engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
            rc = -1;
        }

        if (rc == 0) {
            engine_activate_path(ctx, self, id, "apex_pth");
            idvec_push(&firework_shell, id);
        }

        free(input_path);
        free(explode_path);
        free(apex_path);
    }

    free(characters);
    coordvec_free(&explode_waypoint_coords);
    if (rc == 0 && firework_shell.len > 0) {
        shells_push(st, &firework_shell);
    } else {
        idvec_free(&firework_shell);
    }
    return rc;
}

static int prepare_scenes(Fireworks *st, Effect *self, EngineCtx *ctx) {
    FireworksConfig *cfg = &st->config;

    Gradient final_gradient;
    if (gradient_new(cfg->final_gradient_stops.items, cfg->final_gradient_stops.len,
                     cfg->final_gradient_steps.items, cfg->final_gradient_steps.len, false, false,
                     &final_gradient) != 0) {
        return -1;
    }
    CoordColorMap mapping;
    if (gradient_build_coordinate_color_mapping(&final_gradient, ctx->terminal.canvas.text_bottom,
                                                ctx->terminal.canvas.text_top, ctx->terminal.canvas.text_left,
                                                ctx->terminal.canvas.text_right,
                                                cfg->final_gradient_direction, &mapping) != 0) {
        gradient_free(&final_gradient);
        return -1;
    }

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    size_t chars_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &chars_len);

    size_t arena_len = ctx->terminal.arena.len;
    st->map_len = arena_len;
    st->character_final_color_map = calloc(arena_len ? arena_len : 1, sizeof(ColorPair));
    st->final_present = calloc(arena_len ? arena_len : 1, sizeof(bool));
    for (size_t i = 0; i < chars_len; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        ColorPair pair;
        memset(&pair, 0, sizeof(pair));
        if (dynamic) {
            pair.has_fg = ch->animation.has_input_fg;
            pair.fg = ch->animation.input_fg_color;
            pair.has_bg = ch->animation.has_input_bg;
            pair.bg = ch->animation.input_bg_color;
        } else {
            const Color *mapped = coordcolormap_get(&mapping, ch->input_coord);
            pair.has_fg = true;
            if (mapped) {
                pair.fg = *mapped;
            }
        }
        if ((size_t)id < st->map_len) {
            st->character_final_color_map[id] = pair;
            st->final_present[id] = true;
        }
    }
    free(characters);

    Color white;
    color_from_hex("FFFFFF", &white);

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    int rc = 0;
    for (size_t si = 0; si < st->shells_len && rc == 0; si++) {
        IdVec *shell = &st->shells[si];
        Color shell_color = cfg->firework_colors.items[rng_choice_index(&ctx->rng, cfg->firework_colors.len)];
        Gradient shell_gradient;
        Color sstops[3] = {shell_color, white, shell_color};
        if (gradient_with_steps(sstops, 3, 5, false, &shell_gradient) != 0) {
            rc = -1;
            break;
        }
        for (size_t ii = 0; ii < shell->len && rc == 0; ii++) {
            CharId id = shell->items[ii];
            EffectCharacter *ch = &ctx->terminal.arena.items[id];
            char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
            strcpy(input_symbol, ch->input_symbol);
            bool has_input_fg = ch->animation.has_input_fg;
            Color input_fg = ch->animation.input_fg_color;
            bool has_input_bg = ch->animation.has_input_bg;
            Color input_bg = ch->animation.input_bg_color;
            bool uses_pre = ch->uses_input_preexisting_colors;

            const char *launch_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                         SYNC_DISTANCE, false, no_ease, "", uses_pre);
            Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, launch_scn);
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = shell_color;
            if (scene_add_frame(scene, cfg->firework_symbol, 2, &vp) != 0) {
                rc = -1;
            }
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = white;
            if (rc == 0 && scene_add_frame(scene, cfg->firework_symbol, 1, &vp) != 0) {
                rc = -1;
            }
            if (rc == 0) {
                scene->is_looping = true;
            }

            const char *bloom_scn = NULL;
            if (rc == 0) {
                bloom_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, true, SYNC_STEP,
                                                false, no_ease, "", uses_pre);
                Scene *bloom = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, bloom_scn);
                for (size_t j = 0; j < shell_gradient.len && rc == 0; j++) {
                    VisualParams bvp;
                    memset(&bvp, 0, sizeof(bvp));
                    bvp.has_colors = true;
                    bvp.colors.has_fg = true;
                    bvp.colors.fg = shell_gradient.spectrum[j];
                    if (scene_add_frame(bloom, input_symbol, 2, &bvp) != 0) {
                        rc = -1;
                    }
                }
            }

            const char *fall_scn = NULL;
            if (rc == 0) {
                fall_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                               SYNC_DISTANCE, false, no_ease, "fall_scn", uses_pre);
                Scene *fall = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, fall_scn);
                if (dynamic) {
                    Gradient fg_gradient;
                    Gradient bg_gradient;
                    bool has_fg_grad = false;
                    bool has_bg_grad = false;
                    if (has_input_fg) {
                        Color fstops[2] = {shell_color, input_fg};
                        if (gradient_with_steps(fstops, 2, 15, false, &fg_gradient) != 0) {
                            rc = -1;
                        } else {
                            has_fg_grad = true;
                        }
                    }
                    if (rc == 0 && has_input_bg) {
                        Color bstops[2] = {shell_color, input_bg};
                        if (gradient_with_steps(bstops, 2, 15, false, &bg_gradient) != 0) {
                            rc = -1;
                        } else {
                            has_bg_grad = true;
                        }
                    }
                    if (rc == 0) {
                        if (has_fg_grad || has_bg_grad) {
                            const char *syms[1] = {input_symbol};
                            if (scene_apply_gradient_to_symbols(fall, syms, 1, 10, has_fg_grad ? &fg_gradient : NULL,
                                                                has_bg_grad ? &bg_gradient : NULL) != 0) {
                                rc = -1;
                            }
                        } else {
                            VisualParams dvp;
                            memset(&dvp, 0, sizeof(dvp));
                            dvp.has_colors = true;
                            if (scene_add_frame(fall, input_symbol, 10, &dvp) != 0) {
                                rc = -1;
                            }
                        }
                    }
                    if (has_fg_grad) {
                        gradient_free(&fg_gradient);
                    }
                    if (has_bg_grad) {
                        gradient_free(&bg_gradient);
                    }
                } else {
                    Color final_fg = st->character_final_color_map[id].fg;
                    Gradient fall_gradient;
                    Color fstops[2] = {shell_color, final_fg};
                    if (gradient_with_steps(fstops, 2, 15, false, &fall_gradient) != 0) {
                        rc = -1;
                    } else {
                        const char *syms[1] = {input_symbol};
                        if (scene_apply_gradient_to_symbols(fall, syms, 1, 10, &fall_gradient, NULL) != 0) {
                            rc = -1;
                        }
                        gradient_free(&fall_gradient);
                    }
                }
            }

            if (rc == 0) {
                engine_activate_scene(ctx, self, id, launch_scn);
                CallerKey caller;
                EventAction action;
                memset(&caller, 0, sizeof(caller));
                caller.kind = CALLER_PATH;
                caller.id = "apex_pth";
                memset(&action, 0, sizeof(action));
                action.kind = ACTION_ACTIVATE_SCENE;
                action.has_id = true;
                action.id = (char *)bloom_scn;
                if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
                    rc = -1;
                }
                memset(&caller, 0, sizeof(caller));
                caller.kind = CALLER_PATH;
                caller.id = "input_pth";
                memset(&action, 0, sizeof(action));
                action.kind = ACTION_ACTIVATE_SCENE;
                action.has_id = true;
                action.id = (char *)fall_scn;
                if (rc == 0 && engine_register_event(ctx, id, EVENT_PATH_ACTIVATED, &caller, &action) != 0) {
                    rc = -1;
                }
            }
            free(input_symbol);
        }
        gradient_free(&shell_gradient);
    }

    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    return rc;
}

static int fireworks_build(Effect *self, EngineCtx *ctx) {
    Fireworks *st = self->state;
    FireworksConfig *cfg = &st->config;

    int64_t volume = py_round_half_even(cfg->firework_volume * (double)ctx->terminal.input_characters_len);
    st->firework_volume = volume < 1 ? 1 : volume;
    int64_t dist = py_round_half_even((double)ctx->terminal.canvas.right * cfg->explode_distance);
    if (dist < 1) {
        dist = 1;
    }
    if (dist > 15) {
        dist = 15;
    }
    st->explode_distance = dist;
    st->launch_delay = 0;

    if (prepare_waypoints(st, self, ctx) != 0) {
        return -1;
    }
    return prepare_scenes(st, self, ctx);
}

static const char *fireworks_next_frame(Effect *self, EngineCtx *ctx) {
    Fireworks *st = self->state;
    if (st->shells_len > 0 || !ac_is_empty(&ctx->active_characters)) {
        if (st->shells_len > 0 && st->launch_delay <= 0) {
            IdVec next_group = st->shells[--st->shells_len];
            st->shells[st->shells_len].items = NULL;
            st->shells[st->shells_len].len = 0;
            st->shells[st->shells_len].cap = 0;
            for (size_t i = 0; i < next_group.len; i++) {
                CharId id = next_group.items[i];
                terminal_set_character_visibility(&ctx->terminal, id, true);
                ac_insert(&ctx->active_characters, id);
            }
            idvec_free(&next_group);
            st->launch_delay = (int64_t)((double)st->config.launch_delay * rng_uniform(&ctx->rng, 0.5, 1.5));
        }
        st->launch_delay -= 1;
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void fireworks_destroy(Effect *self) {
    Fireworks *st = self->state;
    if (!st) {
        return;
    }
    for (size_t i = 0; i < st->shells_len; i++) {
        idvec_free(&st->shells[i]);
    }
    free(st->shells);
    free(st->character_final_color_map);
    free(st->final_present);
    free(st);
    free(self);
}

static const EffectOps FIREWORKS_OPS = {fireworks_build, fireworks_next_frame, fireworks_destroy, NULL};

Effect *fireworks_make(const void *cfg) {
    Fireworks *st = calloc(1, sizeof(Fireworks));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const FireworksConfig *)cfg;
    effect->ops = &FIREWORKS_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec fireworks_specs[] = {
    EF_SPEC("explode-anywhere", 0, EF_FLAG, offsetof(FireworksConfig, explode_anywhere)),
    EF_SPEC("firework-colors", 0, EF_COLOR_LIST, offsetof(FireworksConfig, firework_colors)),
    EF_SPEC("firework-symbol", 0, EF_SYMBOL, offsetof(FireworksConfig, firework_symbol)),
    EF_SPEC("firework-volume", 0, EF_RATIO_NONNEG, offsetof(FireworksConfig, firework_volume)),
    EF_SPEC("launch-delay", 0, EF_NONNEG_INT, offsetof(FireworksConfig, launch_delay)),
    EF_SPEC("explode-distance", 0, EF_RATIO_NONNEG, offsetof(FireworksConfig, explode_distance)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(FireworksConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(FireworksConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(FireworksConfig, final_gradient_direction)),
};

const EffectEntry fireworks_entry = {
    "fireworks",
    fireworks_specs,
    sizeof(fireworks_specs) / sizeof(fireworks_specs[0]),
    sizeof(FireworksConfig),
    fireworks_config_defaults,
    fireworks_free_config,
    fireworks_make,
};
