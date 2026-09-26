#include "effects/blackhole.h"

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

typedef enum {
    BH_FORMING,
    BH_CONSUMING,
    BH_COLLAPSING,
    BH_EXPLODING,
    BH_COMPLETE,
} BhPhase;

typedef struct {
    CharId *items;
    size_t len;
    size_t cap;
} IdVec;

typedef struct {
    BlackholeConfig config;
    IdVec blackhole_chars;
    IdVec awaiting_consumption;
    int64_t blackhole_radius;
    Color *character_final_color_map;
    bool *character_final_present;
    size_t map_len;
    int64_t formation_delay;
    int64_t f_delay;
    BhPhase phase;
    IdVec awaiting_blackhole;
} Blackhole;

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

static CharId idvec_remove0(IdVec *v) {
    CharId value = v->items[0];
    for (size_t i = 1; i < v->len; i++) {
        v->items[i - 1] = v->items[i];
    }
    v->len -= 1;
    return value;
}

static IdVec idvec_clone(const IdVec *src) {
    IdVec out;
    out.items = NULL;
    out.len = 0;
    out.cap = 0;
    if (src->len) {
        out.items = malloc(src->len * sizeof(CharId));
        memcpy(out.items, src->items, src->len * sizeof(CharId));
        out.len = src->len;
        out.cap = src->len;
    }
    return out;
}

static void idvec_free(IdVec *v) {
    free(v->items);
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

static int gradient_pair(Color a, Color b, int64_t steps, Gradient *out) {
    Color stops[2] = {a, b};
    return gradient_with_steps(stops, 2, steps, false, out);
}

static bool blackhole_contains(const Blackhole *st, CharId id) {
    for (size_t i = 0; i < st->blackhole_chars.len; i++) {
        if (st->blackhole_chars.items[i] == id) {
            return true;
        }
    }
    return false;
}

void blackhole_config_defaults(void *cfg_ptr) {
    BlackholeConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    color_from_hex("ffffff", &cfg->blackhole_color);
    const char *stars[6] = {"ffcc0d", "ff7326", "ff194d", "bf2669", "702a8c", "049dbf"};
    for (size_t i = 0; i < 6; i++) {
        push_color_default(&cfg->star_colors, stars[i]);
    }
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "ffffff");
    push_int_default(&cfg->final_gradient_steps, 9);
    cfg->final_gradient_direction = GRADIENT_DIAGONAL;
}

void blackhole_free_config(void *cfg_ptr) {
    BlackholeConfig *cfg = cfg_ptr;
    free(cfg->star_colors.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int blackhole_prepare(Blackhole *st, EngineCtx *ctx, Effect *self) {
    static const char *star_symbols[7] = {"*", "'", "`", "¤", "•", "°", "·"};
    Color sc1, sc2;
    color_from_hex("#4a4a4d", &sc1);
    color_from_hex("#ffffff", &sc2);
    Color sf_stops[2] = {sc1, sc2};
    Gradient starfield;
    if (gradient_with_steps(sf_stops, 2, 6, false, &starfield) != 0) {
        return -1;
    }

    IdVec available;
    available.items = NULL;
    available.len = 0;
    available.cap = 0;
    for (size_t i = 0; i < ctx->terminal.input_characters_len; i++) {
        idvec_push(&available, ctx->terminal.input_characters[i]);
    }
    while ((int64_t)st->blackhole_chars.len < st->blackhole_radius * 3 && available.len > 0) {
        int64_t index = rng_randrange(&ctx->rng, 0, (int64_t)available.len);
        idvec_push(&st->blackhole_chars, available.items[index]);
        for (size_t i = (size_t)index; i + 1 < available.len; i++) {
            available.items[i] = available.items[i + 1];
        }
        available.len -= 1;
    }
    idvec_free(&available);

    int64_t count = (int64_t)st->blackhole_chars.len;
    CoordVec ring = find_coords_on_circle(ctx->terminal.canvas.center, st->blackhole_radius, count, true);

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    int rc = 0;
    for (int64_t pi = 0; pi < count && rc == 0; pi++) {
        CharId id = st->blackhole_chars.items[pi];
        Coord starting_pos = ring.items[pi];
        char *blackhole_path = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.7, true,
                            easing_named(EASE_IN_OUT_SINE), false, 0, 0, false, "blackhole",
                            &blackhole_path) != 0) {
            rc = -1;
            break;
        }
        Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, blackhole_path);
        Waypoint wp;
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(p, starting_pos, NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(blackhole_path);
            rc = -1;
            break;
        }
        waypoint_free(&wp);

        bool uses_pre = ctx->terminal.arena.items[id].uses_input_preexisting_colors;
        const char *blackhole_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                        SYNC_DISTANCE, false, no_ease, "blackhole", uses_pre);
        Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, blackhole_scn);
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors.has_fg = true;
        vp.colors.fg = st->config.blackhole_color;
        if (scene_add_frame(scene, "*", 1, &vp) != 0) {
            free(blackhole_path);
            rc = -1;
            break;
        }

        CallerKey caller;
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = blackhole_path;
        EventAction action;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_SET_LAYER;
        action.layer = 1;
        if (engine_register_event(ctx, id, EVENT_PATH_ACTIVATED, &caller, &action) != 0) {
            free(blackhole_path);
            rc = -1;
            break;
        }

        char *rotation_path = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.45, false, no_ease, false, 0, 0, true,
                            "blackhole_rotation", &rotation_path) != 0) {
            free(blackhole_path);
            rc = -1;
            break;
        }
        Path *rp = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, rotation_path);
        for (int64_t k = 0; k < count; k++) {
            Coord coord = ring.items[(pi + k) % count];
            char waypoint_id[32];
            snprintf(waypoint_id, sizeof(waypoint_id), "%zu", rp->waypoints_len);
            memset(&wp, 0, sizeof(wp));
            if (path_new_waypoint(rp, coord, NULL, 0, waypoint_id, &wp) != 0) {
                waypoint_free(&wp);
                free(rotation_path);
                free(blackhole_path);
                rc = -1;
                break;
            }
            waypoint_free(&wp);
        }
        free(rotation_path);
        free(blackhole_path);
    }
    coordvec_free(&ring);
    if (rc != 0) {
        gradient_free(&starfield);
        return -1;
    }

    size_t characters_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &characters_len);
    Coord canvas_center = ctx->terminal.canvas.center;
    Color black;
    color_from_hex("#000000", &black);

    for (size_t i = 0; i < characters_len && rc == 0; i++) {
        CharId id = characters[i];
        terminal_set_character_visibility(&ctx->terminal, id, true);
        const char *star_symbol = star_symbols[rng_choice_index(&ctx->rng, 7)];
        size_t star_color_index = rng_choice_index(&ctx->rng, starfield.len);
        Color star_color = starfield.spectrum[star_color_index];

        bool uses_pre = ctx->terminal.arena.items[id].uses_input_preexisting_colors;
        const char *starting_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                       SYNC_DISTANCE, false, no_ease, "", uses_pre);
        Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, starting_scn);
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors.has_fg = true;
        vp.colors.fg = star_color;
        if (scene_add_frame(scene, star_symbol, 1, &vp) != 0) {
            rc = -1;
            break;
        }
        engine_activate_scene(ctx, self, id, starting_scn);

        if (!blackhole_contains(st, id)) {
            Coord starfield_coord = canvas_random_coord(&ctx->terminal.canvas, &ctx->rng, false, false);
            double speed = rng_uniform(&ctx->rng, 0.17, 0.30);
            motion_set_coordinate(&ctx->terminal.arena.items[id].motion, starfield_coord);
            char *singularity_path = NULL;
            if (motion_new_path(&ctx->terminal.arena.items[id].motion, speed, true, easing_named(EASE_IN_EXPO),
                                false, 0, 0, false, "singularity", &singularity_path) != 0) {
                rc = -1;
                break;
            }
            Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, singularity_path);
            Waypoint wp;
            memset(&wp, 0, sizeof(wp));
            if (path_new_waypoint(p, canvas_center, NULL, 0, "", &wp) != 0) {
                waypoint_free(&wp);
                free(singularity_path);
                rc = -1;
                break;
            }
            waypoint_free(&wp);

            const char *consumed_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                           SYNC_DISTANCE, false, no_ease, "", uses_pre);
            scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, consumed_scn);
            Color gm_stops[2] = {star_color, black};
            Gradient gm;
            if (gradient_with_steps(gm_stops, 2, 10, false, &gm) != 0) {
                free(singularity_path);
                rc = -1;
                break;
            }
            for (size_t j = 0; j < gm.len; j++) {
                VisualParams fvp;
                memset(&fvp, 0, sizeof(fvp));
                fvp.has_colors = true;
                fvp.colors.has_fg = true;
                fvp.colors.fg = gm.spectrum[j];
                if (scene_add_frame(scene, star_symbol, 1, &fvp) != 0) {
                    rc = -1;
                    break;
                }
            }
            gradient_free(&gm);
            if (rc != 0) {
                free(singularity_path);
                break;
            }
            VisualParams dvp;
            memset(&dvp, 0, sizeof(dvp));
            if (scene_add_frame(scene, " ", 1, &dvp) != 0) {
                free(singularity_path);
                rc = -1;
                break;
            }
            scene->has_sync = true;
            scene->sync = SYNC_DISTANCE;

            CallerKey caller;
            memset(&caller, 0, sizeof(caller));
            caller.kind = CALLER_PATH;
            caller.id = singularity_path;
            EventAction action;
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_SET_LAYER;
            action.layer = 2;
            if (engine_register_event(ctx, id, EVENT_PATH_ACTIVATED, &caller, &action) != 0) {
                free(singularity_path);
                rc = -1;
                break;
            }
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_ACTIVATE_SCENE;
            action.has_id = true;
            action.id = (char *)consumed_scn;
            if (engine_register_event(ctx, id, EVENT_PATH_ACTIVATED, &caller, &action) != 0) {
                free(singularity_path);
                rc = -1;
                break;
            }
            idvec_push(&st->awaiting_consumption, id);
            free(singularity_path);
        }
    }
    free(characters);
    gradient_free(&starfield);
    if (rc != 0) {
        return -1;
    }
    rng_shuffle(&ctx->rng, st->awaiting_consumption.items, st->awaiting_consumption.len, sizeof(CharId));
    return 0;
}

static int blackhole_build(Effect *self, EngineCtx *ctx) {
    Blackhole *st = self->state;
    BlackholeConfig *cfg = &st->config;

    int64_t rw = py_round_half_even((double)ctx->terminal.canvas.width * 0.3);
    int64_t rh = py_round_half_even((double)ctx->terminal.canvas.height * 0.20);
    int64_t radius = rw < rh ? rw : rh;
    if (radius < 3) {
        radius = 3;
    }
    st->blackhole_radius = radius;

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

    size_t characters_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &characters_len);
    size_t arena_len = ctx->terminal.arena.len;
    st->map_len = arena_len;
    st->character_final_color_map = calloc(arena_len ? arena_len : 1, sizeof(Color));
    st->character_final_present = calloc(arena_len ? arena_len : 1, sizeof(bool));
    for (size_t i = 0; i < characters_len; i++) {
        CharId id = characters[i];
        Coord input_coord = ctx->terminal.arena.items[id].input_coord;
        const Color *mapped = coordcolormap_get(&final_gradient_mapping, input_coord);
        if ((size_t)id < st->map_len && mapped) {
            st->character_final_color_map[id] = *mapped;
            st->character_final_present[id] = true;
        }
    }
    free(characters);

    int rc = blackhole_prepare(st, ctx, self);

    int64_t bh_len = (int64_t)st->blackhole_chars.len;
    int64_t divisor = bh_len > 0 ? bh_len : 1;
    int64_t formation = py_floor_div(100, divisor);
    if (formation < 6) {
        formation = 6;
    }
    st->formation_delay = formation;
    st->f_delay = formation;
    st->phase = BH_FORMING;
    st->awaiting_blackhole = idvec_clone(&st->blackhole_chars);

    coordcolormap_free(&final_gradient_mapping);
    gradient_free(&final_gradient);
    return rc;
}

static void blackhole_rotate(Blackhole *st, EngineCtx *ctx, Effect *self) {
    IdVec snapshot = idvec_clone(&st->blackhole_chars);
    for (size_t i = 0; i < snapshot.len; i++) {
        engine_activate_path(ctx, self, snapshot.items[i], "blackhole_rotation");
        ac_insert(&ctx->active_characters, snapshot.items[i]);
    }
    idvec_free(&snapshot);
}

static int blackhole_collapse(Blackhole *st, EngineCtx *ctx, Effect *self) {
    static const char *unstable_symbols[7] = {"◦", "◎", "◉", "●", "◉", "◎", "◦"};
    int64_t count = (int64_t)st->blackhole_chars.len;
    Coord center = ctx->terminal.canvas.center;
    CoordVec ring = find_coords_on_circle(center, st->blackhole_radius + 3, count, true);
    bool point_char_made = false;

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    IdVec snapshot = idvec_clone(&st->blackhole_chars);
    int rc = 0;
    for (size_t i = 0; i < snapshot.len && rc == 0; i++) {
        CharId id = snapshot.items[i];
        Coord next_pos = ring.items[0];
        for (size_t j = 1; j < ring.len; j++) {
            ring.items[j - 1] = ring.items[j];
        }
        ring.len -= 1;

        char *expand_path = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.2, true, easing_named(EASE_IN_EXPO), false,
                            0, 0, false, "", &expand_path) != 0) {
            rc = -1;
            break;
        }
        Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, expand_path);
        Waypoint wp;
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(p, next_pos, NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(expand_path);
            rc = -1;
            break;
        }
        waypoint_free(&wp);

        char *collapse_path = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.3, true, easing_named(EASE_IN_EXPO), false,
                            0, 0, false, "", &collapse_path) != 0) {
            free(expand_path);
            rc = -1;
            break;
        }
        p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, collapse_path);
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(p, center, NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(collapse_path);
            free(expand_path);
            rc = -1;
            break;
        }
        waypoint_free(&wp);

        CallerKey caller;
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = expand_path;
        EventAction action;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_PATH;
        action.has_id = true;
        action.id = collapse_path;
        if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
            free(collapse_path);
            free(expand_path);
            rc = -1;
            break;
        }

        if (!point_char_made) {
            bool uses_pre = ctx->terminal.arena.items[id].uses_input_preexisting_colors;
            const char *point_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                        SYNC_DISTANCE, false, no_ease, "", uses_pre);
            Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, point_scn);
            for (int r = 0; r < 3 && rc == 0; r++) {
                for (int s = 0; s < 7; s++) {
                    Color color = st->config.star_colors.items[rng_choice_index(&ctx->rng,
                                                                                st->config.star_colors.len)];
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    vp.colors.has_fg = true;
                    vp.colors.fg = color;
                    if (scene_add_frame(scene, unstable_symbols[s], 3, &vp) != 0) {
                        rc = -1;
                        break;
                    }
                }
            }
            if (rc == 0) {
                CallerKey pc;
                memset(&pc, 0, sizeof(pc));
                pc.kind = CALLER_PATH;
                pc.id = collapse_path;
                memset(&action, 0, sizeof(action));
                action.kind = ACTION_ACTIVATE_SCENE;
                action.has_id = true;
                action.id = (char *)point_scn;
                if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &pc, &action) != 0) {
                    rc = -1;
                }
                memset(&action, 0, sizeof(action));
                action.kind = ACTION_SET_LAYER;
                action.layer = 3;
                if (rc == 0 && engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &pc, &action) != 0) {
                    rc = -1;
                }
            }
            point_char_made = true;
        }

        if (rc == 0) {
            engine_activate_path(ctx, self, id, expand_path);
            ac_insert(&ctx->active_characters, id);
        }
        free(collapse_path);
        free(expand_path);
    }
    idvec_free(&snapshot);
    coordvec_free(&ring);
    return rc;
}

static int blackhole_explode(Blackhole *st, EngineCtx *ctx, Effect *self) {
    Color star_colors[6];
    color_from_hex("#ffcc0d", &star_colors[0]);
    color_from_hex("#ff7326", &star_colors[1]);
    color_from_hex("#ff194d", &star_colors[2]);
    color_from_hex("#bf2669", &star_colors[3]);
    color_from_hex("#702a8c", &star_colors[4]);
    color_from_hex("#049dbf", &star_colors[5]);

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    size_t characters_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &characters_len);

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    int rc = 0;
    for (size_t i = 0; i < characters_len && rc == 0; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        Coord input_coord = ch->input_coord;
        bool has_input_fg = ch->animation.has_input_fg;
        Color input_fg = ch->animation.input_fg_color;
        bool has_input_bg = ch->animation.has_input_bg;
        Color input_bg = ch->animation.input_bg_color;
        bool uses_pre = ch->uses_input_preexisting_colors;
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);

        CoordVec circle = find_coords_on_circle(input_coord, 3, 5, true);
        int64_t nearby_index = rng_randrange(&ctx->rng, 0, 5);
        Coord nearby_coord = circle.items[(size_t)nearby_index];
        coordvec_free(&circle);

        double nearby_speed = (double)rng_randint(&ctx->rng, 3, 4) / 10.0;
        char *nearby_path = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, nearby_speed, true, easing_named(EASE_OUT_EXPO),
                            false, 0, 0, false, "", &nearby_path) != 0) {
            free(input_symbol);
            rc = -1;
            break;
        }
        Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, nearby_path);
        Waypoint wp;
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(p, nearby_coord, NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(nearby_path);
            free(input_symbol);
            rc = -1;
            break;
        }
        waypoint_free(&wp);

        double input_speed = (double)rng_randint(&ctx->rng, 4, 6) / 100.0;
        char *input_path = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, input_speed, true, easing_named(EASE_IN_CUBIC),
                            false, 0, 0, false, "", &input_path) != 0) {
            free(nearby_path);
            free(input_symbol);
            rc = -1;
            break;
        }
        p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, input_path);
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(p, input_coord, NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(input_path);
            free(nearby_path);
            free(input_symbol);
            rc = -1;
            break;
        }
        waypoint_free(&wp);

        Color explode_star_color = star_colors[rng_choice_index(&ctx->rng, 6)];
        const char *explode_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                      SYNC_DISTANCE, false, no_ease, "", uses_pre);
        Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, explode_scn);
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors.has_fg = true;
        vp.colors.fg = explode_star_color;
        if (scene_add_frame(scene, input_symbol, 1, &vp) != 0) {
            free(input_path);
            free(nearby_path);
            free(input_symbol);
            rc = -1;
            break;
        }

        const char *cooling_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                      SYNC_DISTANCE, false, no_ease, "", uses_pre);
        scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, cooling_scn);
        const char *syms[1] = {input_symbol};
        if (dynamic && ctx->preexisting_colors_present) {
            if (!has_input_fg && !has_input_bg) {
                VisualParams dvp;
                memset(&dvp, 0, sizeof(dvp));
                dvp.has_colors = true;
                if (scene_add_frame(scene, input_symbol, 1, &dvp) != 0) {
                    rc = -1;
                }
            } else {
                Gradient cooling_fg, cooling_bg;
                bool has_cooling_fg = false, has_cooling_bg = false;
                if (has_input_fg) {
                    if (gradient_pair(explode_star_color, input_fg, 10, &cooling_fg) != 0) {
                        rc = -1;
                    } else {
                        has_cooling_fg = true;
                    }
                }
                if (rc == 0 && has_input_bg) {
                    if (gradient_pair(explode_star_color, input_bg, 10, &cooling_bg) != 0) {
                        rc = -1;
                    } else {
                        has_cooling_bg = true;
                    }
                }
                if (rc == 0) {
                    if (scene_apply_gradient_to_symbols(scene, syms, 1, 20,
                                                        has_cooling_fg ? &cooling_fg : NULL,
                                                        has_cooling_bg ? &cooling_bg : NULL) != 0) {
                        rc = -1;
                    }
                }
                if (has_cooling_fg) {
                    gradient_free(&cooling_fg);
                }
                if (has_cooling_bg) {
                    gradient_free(&cooling_bg);
                }
            }
        } else {
            Color final_color = st->character_final_color_map[id];
            Gradient cooling;
            if (gradient_pair(explode_star_color, final_color, 10, &cooling) != 0) {
                rc = -1;
            } else {
                if (scene_apply_gradient_to_symbols(scene, syms, 1, 20, &cooling, NULL) != 0) {
                    rc = -1;
                }
                gradient_free(&cooling);
            }
        }

        if (rc == 0) {
            CallerKey caller;
            memset(&caller, 0, sizeof(caller));
            caller.kind = CALLER_PATH;
            caller.id = nearby_path;
            EventAction action;
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_ACTIVATE_PATH;
            action.has_id = true;
            action.id = input_path;
            if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
                rc = -1;
            }
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_ACTIVATE_SCENE;
            action.has_id = true;
            action.id = (char *)cooling_scn;
            if (rc == 0 && engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
                rc = -1;
            }
        }
        if (rc == 0) {
            engine_activate_scene(ctx, self, id, explode_scn);
            engine_activate_path(ctx, self, id, nearby_path);
            ac_insert(&ctx->active_characters, id);
        }

        free(input_path);
        free(nearby_path);
        free(input_symbol);
    }
    free(characters);
    return rc;
}

static const char *blackhole_next_frame(Effect *self, EngineCtx *ctx) {
    Blackhole *st = self->state;
    if (!ac_is_empty(&ctx->active_characters) || st->phase != BH_COMPLETE) {
        switch (st->phase) {
            case BH_FORMING:
                if (st->awaiting_blackhole.len > 0) {
                    if (st->f_delay == 0) {
                        CharId next_char = idvec_remove0(&st->awaiting_blackhole);
                        engine_activate_path(ctx, self, next_char, "blackhole");
                        engine_activate_scene(ctx, self, next_char, "blackhole");
                        ac_insert(&ctx->active_characters, next_char);
                        st->f_delay = st->formation_delay;
                    } else {
                        st->f_delay -= 1;
                    }
                } else if (ac_is_empty(&ctx->active_characters)) {
                    blackhole_rotate(st, ctx, self);
                    st->phase = BH_CONSUMING;
                }
                break;
            case BH_CONSUMING:
                if (st->awaiting_consumption.len > 0) {
                    IdVec snapshot = idvec_clone(&st->awaiting_consumption);
                    for (size_t i = 0; i < snapshot.len; i++) {
                        engine_activate_path(ctx, self, snapshot.items[i], "singularity");
                        ac_insert(&ctx->active_characters, snapshot.items[i]);
                    }
                    idvec_free(&snapshot);
                    st->awaiting_consumption.len = 0;
                } else {
                    CharId *snapshot = NULL;
                    size_t snapshot_len = 0;
                    ac_snapshot(&ctx->active_characters, &snapshot, &snapshot_len);
                    bool all_blackhole = true;
                    for (size_t i = 0; i < snapshot_len; i++) {
                        if (!blackhole_contains(st, snapshot[i])) {
                            all_blackhole = false;
                            break;
                        }
                    }
                    free(snapshot);
                    if (all_blackhole) {
                        st->phase = BH_COLLAPSING;
                    }
                }
                break;
            case BH_COLLAPSING: {
                int rc = blackhole_collapse(st, ctx, self);
                if (rc != 0) {
                    return NULL;
                }
                st->phase = BH_EXPLODING;
                break;
            }
            case BH_EXPLODING: {
                bool all_done = true;
                for (size_t i = 0; i < st->blackhole_chars.len; i++) {
                    EffectCharacter *ch = &ctx->terminal.arena.items[st->blackhole_chars.items[i]];
                    if (ch->motion.active_path != NULL || ch->animation.active_scene != NULL) {
                        all_done = false;
                        break;
                    }
                }
                if (all_done) {
                    int rc = blackhole_explode(st, ctx, self);
                    if (rc != 0) {
                        return NULL;
                    }
                    st->phase = BH_COMPLETE;
                }
                break;
            }
            case BH_COMPLETE:
                break;
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void blackhole_destroy(Effect *self) {
    Blackhole *st = self->state;
    if (!st) {
        return;
    }
    idvec_free(&st->blackhole_chars);
    idvec_free(&st->awaiting_consumption);
    idvec_free(&st->awaiting_blackhole);
    free(st->character_final_color_map);
    free(st->character_final_present);
    free(st);
    free(self);
}

static const EffectOps BLACKHOLE_OPS = {blackhole_build, blackhole_next_frame, blackhole_destroy, NULL};

Effect *blackhole_make(const void *cfg) {
    Blackhole *st = calloc(1, sizeof(Blackhole));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const BlackholeConfig *)cfg;
    effect->ops = &BLACKHOLE_OPS;
    effect->state = st;
    return effect;
}

const EffectEntry blackhole_entry = {
    "blackhole",
    (const EffOptSpec[]){
        EF_SPEC("blackhole-color", 0, EF_COLOR, offsetof(BlackholeConfig, blackhole_color)),
        EF_SPEC("star-colors", 0, EF_COLOR_LIST, offsetof(BlackholeConfig, star_colors)),
        EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(BlackholeConfig, final_gradient_stops)),
        EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(BlackholeConfig, final_gradient_steps)),
        EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(BlackholeConfig, final_gradient_direction)),
    },
    5,
    sizeof(BlackholeConfig),
    blackhole_config_defaults,
    blackhole_free_config,
    blackhole_make,
};
