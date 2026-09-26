#include "effects/swarm.h"

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
#include "utils/ordmap.h"
#include "utils/pycompat.h"

typedef struct {
    CharId *items;
    size_t len;
    size_t cap;
} IdVec;

typedef struct {
    Coord key;
    CoordVec coords;
} AreaEntry;

typedef struct {
    AreaEntry *items;
    size_t len;
    size_t cap;
} AreaMap;

typedef struct {
    SwarmConfig config;
    IdVec *swarms;
    size_t swarms_len;
    size_t swarms_cap;
    ColorPair *character_final_color_map;
    bool *final_present;
    size_t map_len;
    bool call_next;
    char *active_swarm_area;
    IdVec current_swarm;
} Swarm;

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

// Reference Canvas.random_coord(rng, false, false) draws column then row; the
// engine helper's argument evaluation order is unspecified in C, so sequence
// the two draws explicitly to preserve the reference RNG order.
static Coord canvas_random_coord_inside(EngineCtx *ctx) {
    int64_t column = canvas_random_column(&ctx->terminal.canvas, &ctx->rng, false);
    int64_t row = canvas_random_row(&ctx->terminal.canvas, &ctx->rng, false);
    return coord_new(column, row);
}

static int64_t first_char_digit(const char *s) {
    if (!s || s[0] < '0' || s[0] > '9') {
        return 0;
    }
    return s[0] - '0';
}

void swarm_config_defaults(void *cfg_ptr) {
    SwarmConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    push_color_default(&cfg->base_color, "31a0d4");
    color_from_hex("f2ea79", &cfg->flash_color);
    cfg->swarm_size = 0.1;
    cfg->swarm_coordination = 0.80;
    cfg->swarm_area_count_range.start = 2;
    cfg->swarm_area_count_range.end = 4;
    push_color_default(&cfg->final_gradient_stops, "31b900");
    push_color_default(&cfg->final_gradient_stops, "f0ff65");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_HORIZONTAL;
}

void swarm_free_config(void *cfg_ptr) {
    SwarmConfig *cfg = cfg_ptr;
    free(cfg->base_color.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static void area_map_push(AreaMap *m, Coord key, CoordVec coords) {
    if (m->len == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 8;
        AreaEntry *grown = realloc(m->items, cap * sizeof(AreaEntry));
        if (!grown) {
            coordvec_free(&coords);
            return;
        }
        m->items = grown;
        m->cap = cap;
    }
    m->items[m->len].key = key;
    m->items[m->len].coords = coords;
    m->len++;
}

static void area_map_free(AreaMap *m) {
    for (size_t i = 0; i < m->len; i++) {
        coordvec_free(&m->items[i].coords);
    }
    free(m->items);
    m->items = NULL;
    m->len = 0;
    m->cap = 0;
}

static void area_map_set(AreaMap *m, Coord key, CoordVec coords) {
    for (size_t i = 0; i < m->len; i++) {
        if (coord_eq(m->items[i].key, key)) {
            coordvec_free(&m->items[i].coords);
            m->items[i].coords = coords;
            return;
        }
    }
    area_map_push(m, key, coords);
}

static void swarms_push(Swarm *st, IdVec swarm) {
    if (st->swarms_len == st->swarms_cap) {
        size_t cap = st->swarms_cap ? st->swarms_cap * 2 : 8;
        IdVec *grown = realloc(st->swarms, cap * sizeof(IdVec));
        if (!grown) {
            idvec_free(&swarm);
            return;
        }
        st->swarms = grown;
        st->swarms_cap = cap;
    }
    st->swarms[st->swarms_len++] = swarm;
}

static void make_swarms(Swarm *st, EngineCtx *ctx, int64_t swarm_size) {
    size_t n = 0;
    CharId *chars = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                            CS_BOTTOM_TO_TOP_RIGHT_TO_LEFT, &n);
    IdVec unswarmed;
    unswarmed.items = chars;
    unswarmed.len = n;
    unswarmed.cap = n;
    while (unswarmed.len > 0) {
        IdVec new_swarm;
        memset(&new_swarm, 0, sizeof(new_swarm));
        for (int64_t i = 0; i < swarm_size; i++) {
            if (unswarmed.len == 0) {
                break;
            }
            idvec_push(&new_swarm, unswarmed.items[--unswarmed.len]);
        }
        swarms_push(st, new_swarm);
    }
    free(unswarmed.items);
    if (st->swarms_len == 0) {
        return;
    }
    IdVec final_swarm = st->swarms[--st->swarms_len];
    if ((int64_t)final_swarm.len < py_floor_div(swarm_size, 2)) {
        IdVec *last = &st->swarms[st->swarms_len - 1];
        for (size_t i = 0; i < final_swarm.len; i++) {
            idvec_push(last, final_swarm.items[i]);
        }
        idvec_free(&final_swarm);
    } else {
        swarms_push(st, final_swarm);
    }
}

static int swarm_build(Effect *self, EngineCtx *ctx) {
    Swarm *st = self->state;
    SwarmConfig *cfg = &st->config;

    size_t chars_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &chars_len);
    int64_t swarm_size = py_round_half_even((double)chars_len * cfg->swarm_size);
    if (swarm_size < 1) {
        swarm_size = 1;
    }
    make_swarms(st, ctx, swarm_size);

    Gradient final_gradient;
    if (gradient_new(cfg->final_gradient_stops.items, cfg->final_gradient_stops.len,
                     cfg->final_gradient_steps.items, cfg->final_gradient_steps.len, false, false,
                     &final_gradient) != 0) {
        free(characters);
        return -1;
    }
    CoordColorMap mapping;
    if (gradient_build_coordinate_color_mapping(&final_gradient, ctx->terminal.canvas.text_bottom,
                                                ctx->terminal.canvas.text_top, ctx->terminal.canvas.text_left,
                                                ctx->terminal.canvas.text_right,
                                                cfg->final_gradient_direction, &mapping) != 0) {
        gradient_free(&final_gradient);
        free(characters);
        return -1;
    }

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
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

    Color dynamic_clear_color;
    color_from_hex("#ffffff", &dynamic_clear_color);

    int64_t canvas_right = ctx->terminal.canvas.right;
    int64_t canvas_top = ctx->terminal.canvas.top;

    // Effect-local reproduction of upstream's process-wide find_coords_on_circle
    // lru_cache. `swarm_area_coordinate_map` is insertion-ordered with
    // key-overwrite-in-place; the circle cache persists the in-place shuffle.
    OrdMap circle_cache;
    om_init(&circle_cache);

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    int rc = 0;
    for (size_t swarm_index = 0; swarm_index < st->swarms_len && rc == 0; swarm_index++) {
        IdVec swarm;
        swarm.items = malloc(st->swarms[swarm_index].len * sizeof(CharId));
        memcpy(swarm.items, st->swarms[swarm_index].items, st->swarms[swarm_index].len * sizeof(CharId));
        swarm.len = st->swarms[swarm_index].len;
        swarm.cap = swarm.len;

        Color base = cfg->base_color.items[rng_choice_index(&ctx->rng, cfg->base_color.len)];
        Gradient swarm_gradient;
        Color gstops[2] = {base, cfg->flash_color};
        if (gradient_with_steps(gstops, 2, 7, false, &swarm_gradient) != 0) {
            idvec_free(&swarm);
            rc = -1;
            break;
        }
        size_t mirror_len = swarm_gradient.len + 10 + swarm_gradient.len;
        Color *mirror = malloc((mirror_len ? mirror_len : 1) * sizeof(Color));
        size_t mi = 0;
        for (size_t j = 0; j < swarm_gradient.len; j++) {
            mirror[mi++] = swarm_gradient.spectrum[j];
        }
        for (int j = 0; j < 10; j++) {
            mirror[mi++] = cfg->flash_color;
        }
        for (size_t j = swarm_gradient.len; j > 0; j--) {
            mirror[mi++] = swarm_gradient.spectrum[j - 1];
        }

        AreaMap area_map;
        memset(&area_map, 0, sizeof(area_map));

        Coord swarm_spawn = canvas_random_coord(&ctx->terminal.canvas, &ctx->rng, true, false);
        int64_t swarm_area_count = rng_randint(&ctx->rng, cfg->swarm_area_count_range.start,
                                               cfg->swarm_area_count_range.end);
        int64_t radius = py_floor_div(canvas_right < canvas_top ? canvas_right : canvas_top, 2);
        if (radius < 1) {
            radius = 1;
        }
        int64_t area_diameter =
            py_floor_div(canvas_right < canvas_top ? canvas_right : canvas_top, 6);
        if (area_diameter < 1) {
            area_diameter = 1;
        }
        area_diameter *= 2;

        Coord last_focus_coord = swarm_spawn;
        int64_t area_count_made = 0;
        while (area_count_made < swarm_area_count) {
            char keybuf[64];
            snprintf(keybuf, sizeof(keybuf), "%lld,%lld", (long long)last_focus_coord.column,
                     (long long)last_focus_coord.row);
            CoordVec *cached = om_get(&circle_cache, keybuf);
            if (!cached) {
                cached = malloc(sizeof(CoordVec));
                *cached = find_coords_on_circle(last_focus_coord, radius, 0, true);
                om_insert(&circle_cache, keybuf, cached);
            }
            rng_shuffle(&ctx->rng, cached->items, cached->len, sizeof(Coord));
            bool found = false;
            Coord next_focus_coord;
            memset(&next_focus_coord, 0, sizeof(next_focus_coord));
            for (size_t j = 0; j < cached->len; j++) {
                if (canvas_coord_is_in_canvas(&ctx->terminal.canvas, cached->items[j])) {
                    next_focus_coord = cached->items[j];
                    found = true;
                    break;
                }
            }
            if (!found) {
                next_focus_coord = canvas_random_coord_inside(ctx);
            }
            area_count_made++;
            area_map_set(&area_map, last_focus_coord, find_coords_in_circle(last_focus_coord, area_diameter));
            last_focus_coord = next_focus_coord;
        }

        for (size_t si = 0; si < swarm.len && rc == 0; si++) {
            CharId id = swarm.items[si];
            EffectCharacter *ch = &ctx->terminal.arena.items[id];
            Coord input_coord = ch->input_coord;
            bool uses_pre = ch->uses_input_preexisting_colors;
            char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
            strcpy(input_symbol, ch->input_symbol);

            motion_set_coordinate(&ctx->terminal.arena.items[id].motion, swarm_spawn);
            const char *flash_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, true,
                                                        SYNC_DISTANCE, false, no_ease, "", uses_pre);
            Scene *flash_scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, flash_scn);
            for (size_t j = 0; j < mirror_len && rc == 0; j++) {
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                vp.colors.has_fg = true;
                vp.colors.fg = mirror[j];
                if (scene_add_frame(flash_scene, input_symbol, 1, &vp) != 0) {
                    rc = -1;
                }
            }
            if (rc != 0) {
                free(input_symbol);
                break;
            }

            for (size_t k = 0; k < area_map.len && rc == 0; k++) {
                char area_name[64];
                snprintf(area_name, sizeof(area_name), "%zu_swarm_area", k);
                CoordVec *area_coords = &area_map.items[k].coords;
                Coord origin_coord = area_coords->items[rng_choice_index(&ctx->rng, area_coords->len)];

                char *origin_path = NULL;
                if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.4, true, easing_named(EASE_OUT_SINE),
                                    false, 0, 0, false, area_name, &origin_path) != 0) {
                    rc = -1;
                    break;
                }
                Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, origin_path);
                Waypoint wp;
                memset(&wp, 0, sizeof(wp));
                if (path_new_waypoint(p, origin_coord, NULL, 0, area_name, &wp) != 0) {
                    waypoint_free(&wp);
                    free(origin_path);
                    rc = -1;
                    break;
                }
                waypoint_free(&wp);

                CallerKey caller;
                memset(&caller, 0, sizeof(caller));
                caller.kind = CALLER_PATH;
                caller.id = area_name;
                EventAction action;
                memset(&action, 0, sizeof(action));
                action.kind = ACTION_ACTIVATE_SCENE;
                action.has_id = true;
                action.id = (char *)flash_scn;
                if (engine_register_event(ctx, id, EVENT_PATH_ACTIVATED, &caller, &action) != 0) {
                    free(origin_path);
                    rc = -1;
                    break;
                }
                memset(&action, 0, sizeof(action));
                action.kind = ACTION_SET_LAYER;
                action.layer = 1;
                if (engine_register_event(ctx, id, EVENT_PATH_ACTIVATED, &caller, &action) != 0) {
                    free(origin_path);
                    rc = -1;
                    break;
                }
                memset(&action, 0, sizeof(action));
                action.kind = ACTION_DEACTIVATE_SCENE;
                action.has_id = false;
                if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
                    free(origin_path);
                    rc = -1;
                    break;
                }
                free(origin_path);

                for (int inner = 0; inner < 2 && rc == 0; inner++) {
                    Coord next_coord = area_coords->items[rng_choice_index(&ctx->rng, area_coords->len)];
                    char inner_path_id[32];
                    snprintf(inner_path_id, sizeof(inner_path_id), "%zu",
                             om_len(&ctx->terminal.arena.items[id].motion.paths));
                    char *inner_path = NULL;
                    if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.18, true,
                                        easing_named(EASE_IN_OUT_SINE), false, 0, 0, false, inner_path_id,
                                        &inner_path) != 0) {
                        rc = -1;
                        break;
                    }
                    char waypoint_id[32];
                    snprintf(waypoint_id, sizeof(waypoint_id), "%zu",
                             om_len(&ctx->terminal.arena.items[id].motion.paths));
                    p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, inner_path);
                    memset(&wp, 0, sizeof(wp));
                    if (path_new_waypoint(p, next_coord, NULL, 0, waypoint_id, &wp) != 0) {
                        waypoint_free(&wp);
                        free(inner_path);
                        rc = -1;
                        break;
                    }
                    waypoint_free(&wp);
                    free(inner_path);
                }
                if (rc != 0) {
                    break;
                }
            }
            if (rc != 0) {
                free(input_symbol);
                break;
            }

            char *input_path = NULL;
            if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.45, true,
                                easing_named(EASE_IN_OUT_QUAD), false, 0, 0, false, "", &input_path) != 0) {
                free(input_symbol);
                rc = -1;
                break;
            }
            Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, input_path);
            Waypoint wp;
            memset(&wp, 0, sizeof(wp));
            if (path_new_waypoint(p, input_coord, NULL, 0, "", &wp) != 0) {
                waypoint_free(&wp);
                free(input_path);
                free(input_symbol);
                rc = -1;
                break;
            }
            waypoint_free(&wp);

            const char *input_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                        SYNC_DISTANCE, false, no_ease, "", uses_pre);
            ColorPair final_colors = st->character_final_color_map[id];

            if (dynamic) {
                if (!final_colors.has_fg && !final_colors.has_bg) {
                    Gradient clear_gradient;
                    Color cstops[2] = {cfg->flash_color, dynamic_clear_color};
                    if (gradient_with_steps(cstops, 2, 10, false, &clear_gradient) != 0) {
                        free(input_path);
                        free(input_symbol);
                        rc = -1;
                        break;
                    }
                    Scene *scene =
                        (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, input_scn);
                    for (size_t j = 0; j < clear_gradient.len; j++) {
                        VisualParams vp;
                        memset(&vp, 0, sizeof(vp));
                        vp.has_colors = true;
                        vp.colors.has_fg = true;
                        vp.colors.fg = clear_gradient.spectrum[j];
                        if (scene_add_frame(scene, input_symbol, 3, &vp) != 0) {
                            rc = -1;
                            break;
                        }
                    }
                    if (rc == 0) {
                        VisualParams dvp;
                        memset(&dvp, 0, sizeof(dvp));
                        dvp.has_colors = true;
                        if (scene_add_frame(scene, input_symbol, 3, &dvp) != 0) {
                            rc = -1;
                        }
                    }
                    gradient_free(&clear_gradient);
                } else {
                    Gradient fg_gradient;
                    Gradient bg_gradient;
                    bool has_fg_grad = false;
                    bool has_bg_grad = false;
                    if (final_colors.has_fg) {
                        Color fstops[2] = {cfg->flash_color, final_colors.fg};
                        if (gradient_with_steps(fstops, 2, 10, false, &fg_gradient) != 0) {
                            rc = -1;
                        } else {
                            has_fg_grad = true;
                        }
                    }
                    if (rc == 0 && final_colors.has_bg) {
                        Color bstops[2] = {cfg->flash_color, final_colors.bg};
                        if (gradient_with_steps(bstops, 2, 10, false, &bg_gradient) != 0) {
                            rc = -1;
                        } else {
                            has_bg_grad = true;
                        }
                    }
                    if (rc == 0) {
                        Scene *scene =
                            (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, input_scn);
                        const char *syms[1] = {input_symbol};
                        if (scene_apply_gradient_to_symbols(scene, syms, 1, 3, has_fg_grad ? &fg_gradient : NULL,
                                                            has_bg_grad ? &bg_gradient : NULL) != 0) {
                            rc = -1;
                        }
                    }
                    if (has_fg_grad) {
                        gradient_free(&fg_gradient);
                    }
                    if (has_bg_grad) {
                        gradient_free(&bg_gradient);
                    }
                }
            } else {
                Color final_fg = final_colors.fg;
                Gradient landing_gradient;
                Color lstops[2] = {cfg->flash_color, final_fg};
                if (gradient_with_steps(lstops, 2, 10, false, &landing_gradient) != 0) {
                    free(input_path);
                    free(input_symbol);
                    rc = -1;
                    break;
                }
                Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, input_scn);
                for (size_t j = 0; j < landing_gradient.len; j++) {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    vp.colors.has_fg = true;
                    vp.colors.fg = landing_gradient.spectrum[j];
                    if (scene_add_frame(scene, input_symbol, 3, &vp) != 0) {
                        rc = -1;
                        break;
                    }
                }
                gradient_free(&landing_gradient);
            }
            if (rc != 0) {
                free(input_path);
                free(input_symbol);
                break;
            }

            CallerKey caller;
            memset(&caller, 0, sizeof(caller));
            caller.kind = CALLER_PATH;
            caller.id = input_path;
            EventAction action;
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_ACTIVATE_SCENE;
            action.has_id = true;
            action.id = (char *)input_scn;
            if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
                rc = -1;
            }
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_SET_LAYER;
            action.layer = 0;
            if (rc == 0 && engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
                rc = -1;
            }
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_ACTIVATE_SCENE;
            action.has_id = true;
            action.id = (char *)flash_scn;
            if (rc == 0 && engine_register_event(ctx, id, EVENT_PATH_ACTIVATED, &caller, &action) != 0) {
                rc = -1;
            }

            if (rc == 0) {
                size_t n_paths = om_len(&ctx->terminal.arena.items[id].motion.paths);
                const char **all_paths = malloc((n_paths ? n_paths : 1) * sizeof(char *));
                for (size_t j = 0; j < n_paths; j++) {
                    all_paths[j] = om_key_at(&ctx->terminal.arena.items[id].motion.paths, j);
                }
                if (engine_chain_paths(ctx, id, all_paths, n_paths, false) != 0) {
                    rc = -1;
                }
                free(all_paths);
            }

            free(input_path);
            free(input_symbol);
        }

        gradient_free(&swarm_gradient);
        free(mirror);
        area_map_free(&area_map);
        idvec_free(&swarm);
    }

    // Free the circle cache (value vectors are effect-local).
    for (size_t i = 0; i < om_len(&circle_cache); i++) {
        CoordVec *v = (CoordVec *)om_value_at(&circle_cache, i);
        coordvec_free(v);
        free(v);
    }
    om_free(&circle_cache);

    free(characters);
    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    if (rc != 0) {
        return -1;
    }

    st->call_next = true;
    free(st->active_swarm_area);
    st->active_swarm_area = malloc(13);
    strcpy(st->active_swarm_area, "0_swarm_area");
    return 0;
}

static const char *swarm_next_frame(Effect *self, EngineCtx *ctx) {
    Swarm *st = self->state;
    if (st->swarms_len > 0 || !ac_is_empty(&ctx->active_characters)) {
        if (st->swarms_len > 0 && st->call_next) {
            st->call_next = false;
            idvec_free(&st->current_swarm);
            st->current_swarm = st->swarms[--st->swarms_len];
            st->swarms[st->swarms_len].items = NULL;
            st->swarms[st->swarms_len].len = 0;
            st->swarms[st->swarms_len].cap = 0;
            free(st->active_swarm_area);
            st->active_swarm_area = malloc(13);
            strcpy(st->active_swarm_area, "0_swarm_area");
            for (size_t i = 0; i < st->current_swarm.len; i++) {
                CharId id = st->current_swarm.items[i];
                engine_activate_path(ctx, self, id, "0_swarm_area");
                terminal_set_character_visibility(&ctx->terminal, id, true);
                ac_insert(&ctx->active_characters, id);
            }
        }
        if (ac_len(&ctx->active_characters) < st->current_swarm.len) {
            st->call_next = true;
        }
        if (st->current_swarm.len > 0) {
            for (size_t i = 0; i < st->current_swarm.len; i++) {
                CharId id = st->current_swarm.items[i];
                const char *active = ctx->terminal.arena.items[id].motion.active_path;
                if (!active) {
                    continue;
                }
                char *path_id = malloc(strlen(active) + 1);
                strcpy(path_id, active);
                if (strcmp(path_id, st->active_swarm_area) != 0 && strstr(path_id, "swarm_area") != NULL &&
                    first_char_digit(path_id) > first_char_digit(st->active_swarm_area)) {
                    free(st->active_swarm_area);
                    st->active_swarm_area = malloc(strlen(path_id) + 1);
                    strcpy(st->active_swarm_area, path_id);
                    for (size_t j = 0; j < st->current_swarm.len; j++) {
                        CharId other = st->current_swarm.items[j];
                        if (other != id && rng_random(&ctx->rng) < st->config.swarm_coordination) {
                            engine_activate_path(ctx, self, other, st->active_swarm_area);
                        }
                    }
                    free(path_id);
                    break;
                }
                free(path_id);
            }
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void swarm_destroy(Effect *self) {
    Swarm *st = self->state;
    if (!st) {
        return;
    }
    for (size_t i = 0; i < st->swarms_len; i++) {
        idvec_free(&st->swarms[i]);
    }
    free(st->swarms);
    idvec_free(&st->current_swarm);
    free(st->character_final_color_map);
    free(st->final_present);
    free(st->active_swarm_area);
    free(st);
    free(self);
}

static const EffectOps SWARM_OPS = {swarm_build, swarm_next_frame, swarm_destroy, NULL};

Effect *swarm_make(const void *cfg) {
    Swarm *st = calloc(1, sizeof(Swarm));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const SwarmConfig *)cfg;
    st->active_swarm_area = malloc(13);
    strcpy(st->active_swarm_area, "0_swarm_area");
    effect->ops = &SWARM_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec swarm_specs[] = {
    EF_SPEC("base-color", 0, EF_COLOR_LIST, offsetof(SwarmConfig, base_color)),
    EF_SPEC("flash-color", 0, EF_COLOR, offsetof(SwarmConfig, flash_color)),
    EF_SPEC("swarm-size", 0, EF_RATIO_NONNEG, offsetof(SwarmConfig, swarm_size)),
    EF_SPEC("swarm-coordination", 0, EF_RATIO_NONNEG, offsetof(SwarmConfig, swarm_coordination)),
    EF_SPEC("swarm-area-count-range", 0, EF_INT_RANGE, offsetof(SwarmConfig, swarm_area_count_range)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(SwarmConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(SwarmConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(SwarmConfig, final_gradient_direction)),
};

const EffectEntry swarm_entry = {
    "swarm",
    swarm_specs,
    sizeof(swarm_specs) / sizeof(swarm_specs[0]),
    sizeof(SwarmConfig),
    swarm_config_defaults,
    swarm_free_config,
    swarm_make,
};
