#include "effects/bubbles.h"

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

typedef struct {
    CharId *items;
    size_t len;
    size_t cap;
} IdVec;

typedef struct {
    IdVec characters;
    int64_t radius;
    CharId anchor_char;
    int64_t lowest_row;
    bool landed;
} Bubble;

typedef struct {
    Bubble *items;
    size_t len;
    size_t cap;
} BubbleVec;

typedef struct {
    BubblesConfig config;
    BubbleVec bubbles;
    BubbleVec animating_bubbles;
    Gradient rainbow_gradient;
    Color *character_final_color_map;
    bool *final_present;
    size_t map_len;
    int64_t steps_since_last_bubble;
} Bubbles;

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

static void bubble_vec_push(BubbleVec *v, Bubble b) {
    if (v->len == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 8;
        Bubble *grown = realloc(v->items, cap * sizeof(Bubble));
        if (!grown) {
            idvec_free(&b.characters);
            return;
        }
        v->items = grown;
        v->cap = cap;
    }
    v->items[v->len++] = b;
}

static void bubble_vec_free(BubbleVec *v) {
    for (size_t i = 0; i < v->len; i++) {
        idvec_free(&v->items[i].characters);
    }
    free(v->items);
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

static int parse_pop_condition(const char *value, void *dst) {
    PopCondition *p = dst;
    if (strcmp(value, "row") == 0) {
        *p = POP_ROW;
    } else if (strcmp(value, "bottom") == 0) {
        *p = POP_BOTTOM;
    } else if (strcmp(value, "anywhere") == 0) {
        *p = POP_ANYWHERE;
    } else {
        return -1;
    }
    return 0;
}

void bubbles_config_defaults(void *cfg_ptr) {
    BubblesConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->rainbow = false;
    const char *colors[4] = {"d33aff", "7395c4", "43c2a7", "02ff7f"};
    for (size_t i = 0; i < 4; i++) {
        push_color_default(&cfg->bubble_colors, colors[i]);
    }
    color_from_hex("ffffff", &cfg->pop_color);
    cfg->bubble_speed = 0.5;
    cfg->bubble_delay = 20;
    cfg->pop_condition = POP_ROW;
    easing_parse("in_out_sine", &cfg->movement_easing);
    push_color_default(&cfg->final_gradient_stops, "d33aff");
    push_color_default(&cfg->final_gradient_stops, "02ff7f");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_DIAGONAL;
}

void bubbles_free_config(void *cfg_ptr) {
    BubblesConfig *cfg = cfg_ptr;
    free(cfg->bubble_colors.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static void bubble_set_character_coordinates(Bubbles *st, EngineCtx *ctx, Bubble *bubble) {
    Coord anchor_coord = ctx->terminal.arena.items[bubble->anchor_char].motion.current_coord;
    CoordVec points = find_coords_on_circle(anchor_coord, bubble->radius, (int64_t)bubble->characters.len, false);
    for (size_t i = 0; i < bubble->characters.len && i < points.len; i++) {
        CharId id = bubble->characters.items[i];
        Coord point = points.items[i];
        motion_set_coordinate(&ctx->terminal.arena.items[id].motion, point);
        if (point.row == bubble->lowest_row) {
            bubble->landed = true;
        }
    }
    coordvec_free(&points);
    if (st->config.pop_condition == POP_ANYWHERE && rng_random(&ctx->rng) < 0.002) {
        bubble->landed = true;
    }
}

static int make_bubble(Bubbles *st, EngineCtx *ctx, Effect *self, Coord origin, IdVec characters, Bubble *out) {
    int64_t radius = (int64_t)characters.len / 5;
    if (radius < 1) {
        radius = 1;
    }
    CharId anchor_char = terminal_add_character(&ctx->terminal, " ", origin);
    int64_t lowest_row;
    if (st->config.pop_condition == POP_ROW) {
        lowest_row = 0;
        if (characters.len > 0) {
            lowest_row = ctx->terminal.arena.items[characters.items[0]].input_coord.row;
            for (size_t i = 1; i < characters.len; i++) {
                int64_t r = ctx->terminal.arena.items[characters.items[i]].input_coord.row;
                if (r < lowest_row) {
                    lowest_row = r;
                }
            }
        }
    } else {
        lowest_row = ctx->terminal.canvas.bottom;
    }

    Bubble bubble;
    memset(&bubble, 0, sizeof(bubble));
    bubble.characters = characters;
    bubble.radius = radius;
    bubble.anchor_char = anchor_char;
    bubble.lowest_row = lowest_row;
    bubble.landed = false;

    bubble_set_character_coordinates(st, ctx, &bubble);
    bubble.landed = false;

    int64_t waypoint_column = rng_randint(&ctx->rng, ctx->terminal.canvas.left, ctx->terminal.canvas.right);
    char *floor_path = NULL;
    if (motion_new_path(&ctx->terminal.arena.items[bubble.anchor_char].motion, st->config.bubble_speed, false,
                        easing_named(EASE_LINEAR), false, 0, 0, false, "", &floor_path) != 0) {
        idvec_free(&bubble.characters);
        return -1;
    }
    Path *fp = (Path *)om_get(&ctx->terminal.arena.items[bubble.anchor_char].motion.paths, floor_path);
    Waypoint wp;
    memset(&wp, 0, sizeof(wp));
    if (path_new_waypoint(fp, coord_new(waypoint_column, bubble.lowest_row), NULL, 0, "", &wp) != 0) {
        waypoint_free(&wp);
        free(floor_path);
        idvec_free(&bubble.characters);
        return -1;
    }
    waypoint_free(&wp);
    free(floor_path);
    engine_activate_path(ctx, self, bubble.anchor_char, "0");

    if (st->config.rainbow) {
        size_t glen = st->rainbow_gradient.len;
        Color *local = malloc((glen ? glen : 1) * sizeof(Color));
        memcpy(local, st->rainbow_gradient.spectrum, glen * sizeof(Color));
        size_t gradient_offset = 0;
        for (size_t i = 0; i < bubble.characters.len; i++) {
            CharId id = bubble.characters.items[i];
            EffectCharacter *ch = &ctx->terminal.arena.items[id];
            char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
            strcpy(input_symbol, ch->input_symbol);
            bool uses_pre = ch->uses_input_preexisting_colors;

            const char *sheen_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                        SYNC_DISTANCE, false, easing_named(EASE_LINEAR), "",
                                                        uses_pre);
            Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, sheen_scn);
            for (size_t j = 0; j < glen; j++) {
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                vp.colors.has_fg = true;
                vp.colors.fg = local[j];
                if (scene_add_frame(scene, input_symbol, 4, &vp) != 0) {
                    free(input_symbol);
                    free(local);
                    idvec_free(&bubble.characters);
                    return -1;
                }
            }
            free(input_symbol);

            gradient_offset += 2;
            gradient_offset %= glen;
            Color *rotated = malloc((glen ? glen : 1) * sizeof(Color));
            for (size_t j = 0; j < glen; j++) {
                rotated[j] = local[(gradient_offset + j) % glen];
            }
            memcpy(local, rotated, glen * sizeof(Color));
            free(rotated);

            engine_activate_scene(ctx, self, id, sheen_scn);
            EffectCharacter *ch2 = &ctx->terminal.arena.items[id];
            if (ch2->animation.active_scene) {
                Scene *active = (Scene *)om_get(&ch2->animation.scenes, ch2->animation.active_scene);
                if (active) {
                    active->is_looping = true;
                }
            }
        }
        free(local);
    } else {
        Color bubble_color = st->config.bubble_colors.items[rng_choice_index(&ctx->rng,
                                                                             st->config.bubble_colors.len)];
        for (size_t i = 0; i < bubble.characters.len; i++) {
            CharId id = bubble.characters.items[i];
            EffectCharacter *ch = &ctx->terminal.arena.items[id];
            char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
            strcpy(input_symbol, ch->input_symbol);
            bool uses_pre = ch->uses_input_preexisting_colors;

            const char *sheen_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                        SYNC_DISTANCE, false, easing_named(EASE_LINEAR), "",
                                                        uses_pre);
            Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, sheen_scn);
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = bubble_color;
            if (scene_add_frame(scene, input_symbol, 1, &vp) != 0) {
                free(input_symbol);
                idvec_free(&bubble.characters);
                return -1;
            }
            free(input_symbol);
            engine_activate_scene(ctx, self, id, sheen_scn);
        }
    }

    *out = bubble;
    return 0;
}

static void bubble_pop(Bubbles *st, EngineCtx *ctx, Effect *self, Bubble *bubble) {
    Coord anchor_coord = ctx->terminal.arena.items[bubble->anchor_char].motion.current_coord;
    CoordVec points = find_coords_on_circle(anchor_coord, bubble->radius + 3, (int64_t)bubble->characters.len, true);
    for (size_t i = 0; i < bubble->characters.len && i < points.len; i++) {
        CharId id = bubble->characters.items[i];
        Coord point = points.items[i];
        char *pop_out_path = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.3, true, easing_named(EASE_OUT_EXPO), false, 0,
                            0, false, "pop_out", &pop_out_path) != 0) {
            continue;
        }
        Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, pop_out_path);
        Waypoint wp;
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(p, point, NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(pop_out_path);
            continue;
        }
        waypoint_free(&wp);
        CallerKey caller;
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = "pop_out";
        EventAction action;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_PATH;
        action.has_id = true;
        action.id = "final";
        if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
            free(pop_out_path);
            continue;
        }
        free(pop_out_path);
    }
    coordvec_free(&points);
    for (size_t i = 0; i < bubble->characters.len; i++) {
        CharId id = bubble->characters.items[i];
        engine_activate_scene(ctx, self, id, "pop_1");
        engine_activate_path(ctx, self, id, "pop_out");
    }
    (void)st;
}

static void bubble_move(Bubbles *st, EngineCtx *ctx, Effect *self, Bubble *bubble) {
    engine_motion_move(ctx, self, bubble->anchor_char);
    bubble_set_character_coordinates(st, ctx, bubble);
    for (size_t i = 0; i < bubble->characters.len; i++) {
        engine_step_animation(ctx, self, bubble->characters.items[i]);
    }
}

static int bubbles_build(Effect *self, EngineCtx *ctx) {
    Bubbles *st = self->state;
    BubblesConfig *cfg = &st->config;

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
    size_t n = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);

    size_t arena_len = ctx->terminal.arena.len;
    st->map_len = arena_len;
    st->character_final_color_map = calloc(arena_len ? arena_len : 1, sizeof(Color));
    st->final_present = calloc(arena_len ? arena_len : 1, sizeof(bool));

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
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

        const Color *mapped = coordcolormap_get(&mapping, input_coord);
        Color final_color;
        if (mapped) {
            final_color = *mapped;
        } else {
            memset(&final_color, 0, sizeof(final_color));
        }
        if ((size_t)id < st->map_len) {
            st->character_final_color_map[id] = final_color;
            st->final_present[id] = true;
        }
        ctx->terminal.arena.items[id].layer = 1;

        const char *pop_1_scene = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                      SYNC_DISTANCE, false, no_ease, "pop_1", uses_pre);
        const char *pop_2_scene = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                      SYNC_DISTANCE, false, no_ease, "", uses_pre);
        Scene *s1 = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, pop_1_scene);
        VisualParams pvp;
        memset(&pvp, 0, sizeof(pvp));
        pvp.has_colors = true;
        pvp.colors.has_fg = true;
        pvp.colors.fg = cfg->pop_color;
        if (scene_add_frame(s1, "*", 9, &pvp) != 0) {
            rc = -1;
        }
        Scene *s2 = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, pop_2_scene);
        if (rc == 0 && scene_add_frame(s2, "'", 9, &pvp) != 0) {
            rc = -1;
        }

        const char *final_scene = NULL;
        if (rc == 0) {
            final_scene = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false, SYNC_DISTANCE,
                                              false, no_ease, "", uses_pre);
            Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, final_scene);
            if (dynamic) {
                Gradient fg_gradient;
                Gradient bg_gradient;
                bool has_fg_grad = false;
                bool has_bg_grad = false;
                if (has_input_fg) {
                    Color fstops[2] = {cfg->pop_color, input_fg};
                    if (gradient_with_steps(fstops, 2, 8, false, &fg_gradient) != 0) {
                        rc = -1;
                    } else {
                        has_fg_grad = true;
                    }
                }
                if (rc == 0 && has_input_bg) {
                    Color bstops[2] = {cfg->pop_color, input_bg};
                    if (gradient_with_steps(bstops, 2, 8, false, &bg_gradient) != 0) {
                        rc = -1;
                    } else {
                        has_bg_grad = true;
                    }
                }
                if (rc == 0) {
                    if (has_fg_grad || has_bg_grad) {
                        const char *syms[1] = {input_symbol};
                        if (scene_apply_gradient_to_symbols(scene, syms, 1, 6, has_fg_grad ? &fg_gradient : NULL,
                                                            has_bg_grad ? &bg_gradient : NULL) != 0) {
                            rc = -1;
                        }
                    } else {
                        VisualParams dvp;
                        memset(&dvp, 0, sizeof(dvp));
                        dvp.has_colors = true;
                        if (scene_add_frame(scene, input_symbol, 6, &dvp) != 0) {
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
                Color final_color_stored = st->character_final_color_map[id];
                Gradient char_final_gradient;
                Color cstops[2] = {cfg->pop_color, final_color_stored};
                if (gradient_with_steps(cstops, 2, 8, false, &char_final_gradient) != 0) {
                    rc = -1;
                } else {
                    const char *syms[1] = {input_symbol};
                    if (scene_apply_gradient_to_symbols(scene, syms, 1, 6, &char_final_gradient, NULL) != 0) {
                        rc = -1;
                    }
                    gradient_free(&char_final_gradient);
                }
            }
        }

        if (rc == 0) {
            CallerKey caller;
            EventAction action;
            memset(&caller, 0, sizeof(caller));
            caller.kind = CALLER_SCENE;
            caller.id = (char *)pop_1_scene;
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_ACTIVATE_SCENE;
            action.has_id = true;
            action.id = (char *)pop_2_scene;
            if (engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
                rc = -1;
            }
            memset(&caller, 0, sizeof(caller));
            caller.kind = CALLER_SCENE;
            caller.id = (char *)pop_2_scene;
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_ACTIVATE_SCENE;
            action.has_id = true;
            action.id = (char *)final_scene;
            if (rc == 0 && engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
                rc = -1;
            }
        }

        if (rc == 0) {
            char *final_path = NULL;
            if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.3, true, easing_named(EASE_IN_OUT_EXPO),
                                false, 0, 0, false, "final", &final_path) != 0) {
                rc = -1;
            } else {
                Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, final_path);
                Waypoint wp;
                memset(&wp, 0, sizeof(wp));
                if (path_new_waypoint(p, input_coord, NULL, 0, "", &wp) != 0) {
                    waypoint_free(&wp);
                    rc = -1;
                } else {
                    waypoint_free(&wp);
                    CallerKey caller;
                    memset(&caller, 0, sizeof(caller));
                    caller.kind = CALLER_PATH;
                    caller.id = "final";
                    EventAction action;
                    memset(&action, 0, sizeof(action));
                    action.kind = ACTION_SET_LAYER;
                    action.layer = 0;
                    if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
                        rc = -1;
                    }
                }
                free(final_path);
            }
        }
        free(input_symbol);
    }
    free(characters);

    if (rc == 0) {
        IdVec unbubbled;
        memset(&unbubbled, 0, sizeof(unbubbled));
        CharIdGrouping grouped = terminal_get_characters_grouped(&ctx->terminal, character_filter_default(),
                                                                 CG_ROW_BOTTOM_TO_TOP);
        for (size_t b = 0; b < grouped.len; b++) {
            for (size_t j = 0; j < grouped.buckets[b].len; j++) {
                idvec_push(&unbubbled, grouped.buckets[b].items[j]);
            }
        }
        charidgrouping_free(&grouped);

        bubble_vec_free(&st->bubbles);
        while (unbubbled.len > 0) {
            IdVec bubble_group;
            memset(&bubble_group, 0, sizeof(bubble_group));
            if (unbubbled.len < 5) {
                bubble_group = unbubbled;
                memset(&unbubbled, 0, sizeof(unbubbled));
            } else {
                int64_t max_count = (int64_t)unbubbled.len < 20 ? (int64_t)unbubbled.len : 20;
                int64_t count = rng_randint(&ctx->rng, 5, max_count);
                for (int64_t k = 0; k < count; k++) {
                    idvec_push(&bubble_group, unbubbled.items[0]);
                    for (size_t j = 1; j < unbubbled.len; j++) {
                        unbubbled.items[j - 1] = unbubbled.items[j];
                    }
                    unbubbled.len--;
                }
            }
            Coord bubble_origin = coord_new(rng_randint(&ctx->rng, ctx->terminal.canvas.left,
                                                        ctx->terminal.canvas.right),
                                            ctx->terminal.canvas.top + 10);
            Bubble new_bubble;
            if (make_bubble(st, ctx, self, bubble_origin, bubble_group, &new_bubble) != 0) {
                idvec_free(&unbubbled);
                rc = -1;
                break;
            }
            bubble_vec_push(&st->bubbles, new_bubble);
        }
        if (rc != 0) {
            idvec_free(&unbubbled);
        }
    }

    bubble_vec_free(&st->animating_bubbles);
    st->steps_since_last_bubble = 0;
    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    return rc;
}

static const char *bubbles_next_frame(Effect *self, EngineCtx *ctx) {
    Bubbles *st = self->state;
    if (st->animating_bubbles.len > 0 || !ac_is_empty(&ctx->active_characters) || st->bubbles.len > 0) {
        if (st->bubbles.len > 0 && st->steps_since_last_bubble >= st->config.bubble_delay) {
            Bubble next_bubble = st->bubbles.items[0];
            for (size_t j = 1; j < st->bubbles.len; j++) {
                st->bubbles.items[j - 1] = st->bubbles.items[j];
            }
            st->bubbles.len--;
            for (size_t i = 0; i < next_bubble.characters.len; i++) {
                terminal_set_character_visibility(&ctx->terminal, next_bubble.characters.items[i], true);
            }
            bubble_vec_push(&st->animating_bubbles, next_bubble);
            st->steps_since_last_bubble = 0;
        }
        st->steps_since_last_bubble += 1;

        BubbleVec animating = st->animating_bubbles;
        memset(&st->animating_bubbles, 0, sizeof(st->animating_bubbles));
        for (size_t i = 0; i < animating.len; i++) {
            Bubble *bubble = &animating.items[i];
            if (bubble->landed) {
                bubble_pop(st, ctx, self, bubble);
                for (size_t j = 0; j < bubble->characters.len; j++) {
                    ac_insert(&ctx->active_characters, bubble->characters.items[j]);
                }
            }
        }

        BubbleVec keep;
        memset(&keep, 0, sizeof(keep));
        for (size_t i = 0; i < animating.len; i++) {
            if (animating.items[i].landed) {
                idvec_free(&animating.items[i].characters);
            } else {
                bubble_vec_push(&keep, animating.items[i]);
            }
        }
        free(animating.items);

        for (size_t i = 0; i < keep.len; i++) {
            bubble_move(st, ctx, self, &keep.items[i]);
        }
        st->animating_bubbles = keep;

        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void bubbles_destroy(Effect *self) {
    Bubbles *st = self->state;
    if (!st) {
        return;
    }
    bubble_vec_free(&st->bubbles);
    bubble_vec_free(&st->animating_bubbles);
    gradient_free(&st->rainbow_gradient);
    free(st->character_final_color_map);
    free(st->final_present);
    free(st);
    free(self);
}

static const EffectOps BUBBLES_OPS = {bubbles_build, bubbles_next_frame, bubbles_destroy, NULL};

Effect *bubbles_make(const void *cfg) {
    Bubbles *st = calloc(1, sizeof(Bubbles));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const BubblesConfig *)cfg;
    const char *rainbow_stops[7] = {"e81416", "ffa500", "faeb36", "79c314", "487de7", "4b369d", "70369d"};
    Color stops[7];
    for (size_t i = 0; i < 7; i++) {
        color_from_hex(rainbow_stops[i], &stops[i]);
    }
    if (gradient_with_steps(stops, 7, 5, false, &st->rainbow_gradient) != 0) {
        free(st);
        free(effect);
        return NULL;
    }
    effect->ops = &BUBBLES_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec bubbles_specs[] = {
    EF_SPEC("rainbow", 0, EF_FLAG, offsetof(BubblesConfig, rainbow)),
    EF_SPEC("bubble-colors", 0, EF_COLOR_LIST, offsetof(BubblesConfig, bubble_colors)),
    EF_SPEC("pop-color", 0, EF_COLOR, offsetof(BubblesConfig, pop_color)),
    EF_SPEC("bubble-speed", 0, EF_FLOAT_POS, offsetof(BubblesConfig, bubble_speed)),
    EF_SPEC("bubble-delay", 0, EF_POS_INT, offsetof(BubblesConfig, bubble_delay)),
    {"pop-condition", 0, EF_CUSTOM, offsetof(BubblesConfig, pop_condition), parse_pop_condition},
    EF_SPEC("movement-easing", 0, EF_EASING, offsetof(BubblesConfig, movement_easing)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(BubblesConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(BubblesConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(BubblesConfig, final_gradient_direction)),
};

const EffectEntry bubbles_entry = {
    "bubbles",
    bubbles_specs,
    sizeof(bubbles_specs) / sizeof(bubbles_specs[0]),
    sizeof(BubblesConfig),
    bubbles_config_defaults,
    bubbles_free_config,
    bubbles_make,
};
