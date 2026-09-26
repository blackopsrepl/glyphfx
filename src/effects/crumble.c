#include "effects/crumble.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
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
    CRUMBLE_FALLING,
    CRUMBLE_VACUUMING,
    CRUMBLE_RESETTING,
    CRUMBLE_COMPLETE,
} CrumbleStage;

typedef struct {
    CharId *items;
    size_t len;
    size_t cap;
} IdVec;

typedef struct {
    CrumbleConfig config;
    IdVec pending;
    Color *character_final_color_map;
    bool *character_final_present;
    size_t map_len;
    int64_t fall_delay;
    int64_t max_fall_delay;
    int64_t min_fall_delay;
    bool reset;
    int64_t fall_group_maxsize;
    CrumbleStage stage;
    IdVec unvacuumed;
} Crumble;

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

void crumble_config_defaults(void *cfg_ptr) {
    CrumbleConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    push_color_default(&cfg->final_gradient_stops, "5CE1FF");
    push_color_default(&cfg->final_gradient_stops, "FF8C00");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_DIAGONAL;
}

void crumble_free_config(void *cfg_ptr) {
    CrumbleConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int crumble_build(Effect *self, EngineCtx *ctx) {
    Crumble *st = self->state;
    CrumbleConfig *cfg = &st->config;

    Color dynamic_neutral_gray;
    color_from_hex("#808080", &dynamic_neutral_gray);
    Color white;
    color_from_hex("#ffffff", &white);

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
    size_t characters_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &characters_len);
    int64_t canvas_bottom = ctx->terminal.canvas.bottom;
    int64_t canvas_top = ctx->terminal.canvas.top;
    int64_t canvas_center_column = ctx->terminal.canvas.center_column;
    int64_t canvas_center_row = ctx->terminal.canvas.center_row;

    size_t arena_len = ctx->terminal.arena.len;
    st->map_len = arena_len;
    st->character_final_color_map = calloc(arena_len ? arena_len : 1, sizeof(Color));
    st->character_final_present = calloc(arena_len ? arena_len : 1, sizeof(bool));

    static const char *dust_symbols[3] = {"*", ".", ","};

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

        const Color *mapped = coordcolormap_get(&final_gradient_mapping, input_coord);
        Color final_color;
        if (mapped) {
            final_color = *mapped;
        } else {
            memset(&final_color, 0, sizeof(final_color));
        }
        if ((size_t)id < st->map_len) {
            st->character_final_color_map[id] = final_color;
            st->character_final_present[id] = true;
        }

        Color weak_fg, weak_bg, dust_fg, dust_bg;
        bool has_weak_fg = false, has_weak_bg = false, has_dust_fg = false, has_dust_bg = false;
        Gradient flash_fg, flash_bg, str_fg, str_bg;
        bool has_flash_fg = false, has_flash_bg = false, has_str_fg = false, has_str_bg = false;

        if (dynamic) {
            bool has_existing = has_input_fg || has_input_bg;
            if (has_input_fg) {
                weak_fg = color_adjust_brightness(&input_fg, 0.65);
                has_weak_fg = true;
            } else if (!has_input_bg) {
                weak_fg = color_adjust_brightness(&dynamic_neutral_gray, 0.65);
                has_weak_fg = true;
            }
            if (has_input_bg) {
                weak_bg = color_adjust_brightness(&input_bg, 0.65);
                has_weak_bg = true;
            }
            if (has_input_fg) {
                dust_fg = color_adjust_brightness(&input_fg, 0.55);
                has_dust_fg = true;
            } else if (!has_input_bg) {
                dust_fg = color_adjust_brightness(&dynamic_neutral_gray, 0.55);
                has_dust_fg = true;
            }
            if (has_input_bg) {
                dust_bg = color_adjust_brightness(&input_bg, 0.55);
                has_dust_bg = true;
            }
            if (has_input_fg) {
                if (gradient_pair(input_fg, white, 6, &flash_fg) != 0) {
                    rc = -1;
                } else {
                    has_flash_fg = true;
                }
            } else if (!has_existing) {
                if (gradient_pair(dynamic_neutral_gray, white, 6, &flash_fg) != 0) {
                    rc = -1;
                } else {
                    has_flash_fg = true;
                }
            }
            if (rc == 0 && has_input_bg) {
                if (gradient_pair(input_bg, white, 6, &flash_bg) != 0) {
                    rc = -1;
                } else {
                    has_flash_bg = true;
                }
            }
            if (rc == 0 && has_input_fg) {
                if (gradient_pair(white, input_fg, 9, &str_fg) != 0) {
                    rc = -1;
                } else {
                    has_str_fg = true;
                }
            }
            if (rc == 0 && has_input_bg) {
                if (gradient_pair(white, input_bg, 9, &str_bg) != 0) {
                    rc = -1;
                } else {
                    has_str_bg = true;
                }
            }
        } else {
            weak_fg = color_adjust_brightness(&final_color, 0.65);
            has_weak_fg = true;
            dust_fg = color_adjust_brightness(&final_color, 0.55);
            has_dust_fg = true;
            if (gradient_pair(final_color, white, 6, &flash_fg) != 0) {
                rc = -1;
            } else {
                has_flash_fg = true;
            }
            if (rc == 0) {
                if (gradient_pair(white, final_color, 9, &str_fg) != 0) {
                    rc = -1;
                } else {
                    has_str_fg = true;
                }
            }
        }

        Gradient weaken_fg, weaken_bg;
        bool has_weaken_fg = false, has_weaken_bg = false;
        if (rc == 0 && has_weak_fg && has_dust_fg) {
            if (gradient_pair(weak_fg, dust_fg, 9, &weaken_fg) != 0) {
                rc = -1;
            } else {
                has_weaken_fg = true;
            }
        }
        if (rc == 0 && has_weak_bg && has_dust_bg) {
            if (gradient_pair(weak_bg, dust_bg, 9, &weaken_bg) != 0) {
                rc = -1;
            } else {
                has_weaken_bg = true;
            }
        }

        if (rc != 0) {
            free(input_symbol);
            break;
        }

        terminal_set_character_visibility(&ctx->terminal, id, true);

        const char *initial_scn =
            animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false, SYNC_DISTANCE, false,
                                no_ease, "", uses_pre);
        Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, initial_scn);
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors.has_fg = has_weak_fg;
        vp.colors.fg = weak_fg;
        vp.colors.has_bg = has_weak_bg;
        vp.colors.bg = weak_bg;
        if (scene_add_frame(scene, input_symbol, 1, &vp) != 0) {
            rc = -1;
        }
        if (rc == 0) {
            engine_activate_scene(ctx, self, id, initial_scn);
        }

        char *fall_path = NULL;
        if (rc == 0) {
            if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.65, true, easing_named(EASE_OUT_BOUNCE),
                                false, 0, 0, false, "", &fall_path) != 0) {
                rc = -1;
            } else {
                Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, fall_path);
                Waypoint wp;
                memset(&wp, 0, sizeof(wp));
                if (path_new_waypoint(p, coord_new(input_coord.column, canvas_bottom), NULL, 0, "", &wp) != 0) {
                    waypoint_free(&wp);
                    rc = -1;
                } else {
                    waypoint_free(&wp);
                }
            }
        }

        const char *weaken_scn = NULL;
        if (rc == 0) {
            weaken_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false, SYNC_DISTANCE,
                                             false, no_ease, "weaken", uses_pre);
            scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, weaken_scn);
            const char *syms[1] = {input_symbol};
            if (scene_apply_gradient_to_symbols(scene, syms, 1, 4, has_weaken_fg ? &weaken_fg : NULL,
                                                has_weaken_bg ? &weaken_bg : NULL) != 0) {
                rc = -1;
            }
        }

        if (rc == 0) {
            char *top_path = NULL;
            if (motion_new_path(&ctx->terminal.arena.items[id].motion, 1.0, true, easing_named(EASE_OUT_QUINT),
                                false, 0, 0, false, "top", &top_path) != 0) {
                rc = -1;
            } else {
                free(top_path);
                Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, "top");
                Coord bez[1] = {coord_new(canvas_center_column, canvas_center_row)};
                Waypoint wp;
                memset(&wp, 0, sizeof(wp));
                if (path_new_waypoint(p, coord_new(input_coord.column, canvas_top), bez, 1, "", &wp) != 0) {
                    waypoint_free(&wp);
                    rc = -1;
                } else {
                    waypoint_free(&wp);
                }
            }
        }

        if (rc == 0) {
            if (motion_new_path(&ctx->terminal.arena.items[id].motion, 1.0, false, no_ease, false, 0, 0, false,
                                "input", NULL) != 0) {
                rc = -1;
            } else {
                Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, "input");
                Waypoint wp;
                memset(&wp, 0, sizeof(wp));
                if (path_new_waypoint(p, input_coord, NULL, 0, "", &wp) != 0) {
                    waypoint_free(&wp);
                    rc = -1;
                } else {
                    waypoint_free(&wp);
                }
            }
        }

        const char *flash_scn = NULL;
        if (rc == 0) {
            flash_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false, SYNC_DISTANCE,
                                            false, no_ease, "", uses_pre);
            scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, flash_scn);
            const char *syms[1] = {input_symbol};
            if (scene_apply_gradient_to_symbols(scene, syms, 1, 4, has_flash_fg ? &flash_fg : NULL,
                                                has_flash_bg ? &flash_bg : NULL) != 0) {
                rc = -1;
            }
        }

        const char *str_scn = NULL;
        if (rc == 0) {
            str_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false, SYNC_DISTANCE,
                                          false, no_ease, "", uses_pre);
            scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, str_scn);
            if (dynamic && !has_input_fg && !has_input_bg) {
                VisualParams dvp;
                memset(&dvp, 0, sizeof(dvp));
                dvp.has_colors = true;
                if (scene_add_frame(scene, input_symbol, 4, &dvp) != 0) {
                    rc = -1;
                }
            } else {
                const char *syms[1] = {input_symbol};
                if (scene_apply_gradient_to_symbols(scene, syms, 1, 4, has_str_fg ? &str_fg : NULL,
                                                    has_str_bg ? &str_bg : NULL) != 0) {
                    rc = -1;
                }
            }
        }

        const char *dust_scn = NULL;
        if (rc == 0) {
            dust_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, true, SYNC_DISTANCE,
                                           false, no_ease, "", uses_pre);
            scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, dust_scn);
            for (int k = 0; k < 5 && rc == 0; k++) {
                const char *symbol = dust_symbols[rng_choice_index(&ctx->rng, 3)];
                VisualParams dvp;
                memset(&dvp, 0, sizeof(dvp));
                dvp.has_colors = true;
                dvp.colors.has_fg = has_dust_fg;
                dvp.colors.fg = dust_fg;
                dvp.colors.has_bg = has_dust_bg;
                dvp.colors.bg = dust_bg;
                if (scene_add_frame(scene, symbol, 1, &dvp) != 0) {
                    rc = -1;
                }
            }
        }

        if (rc == 0) {
            CallerKey caller;
            memset(&caller, 0, sizeof(caller));
            caller.kind = CALLER_SCENE;
            caller.id = "weaken";
            EventAction action;
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_ACTIVATE_PATH;
            action.has_id = true;
            action.id = fall_path;
            if (engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
                rc = -1;
            }
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_SET_LAYER;
            action.layer = 1;
            if (rc == 0 && engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
                rc = -1;
            }
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_ACTIVATE_SCENE;
            action.has_id = true;
            action.id = (char *)dust_scn;
            if (rc == 0 && engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
                rc = -1;
            }

            CallerKey path_caller;
            memset(&path_caller, 0, sizeof(path_caller));
            path_caller.kind = CALLER_PATH;
            path_caller.id = "input";
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_ACTIVATE_SCENE;
            action.has_id = true;
            action.id = (char *)flash_scn;
            if (rc == 0 && engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &path_caller, &action) != 0) {
                rc = -1;
            }

            CallerKey flash_caller;
            memset(&flash_caller, 0, sizeof(flash_caller));
            flash_caller.kind = CALLER_SCENE;
            flash_caller.id = (char *)flash_scn;
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_ACTIVATE_SCENE;
            action.has_id = true;
            action.id = (char *)str_scn;
            if (rc == 0 && engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &flash_caller, &action) != 0) {
                rc = -1;
            }
        }

        if (rc == 0) {
            idvec_push(&st->pending, id);
        }

        free(fall_path);
        free(input_symbol);
        if (has_weaken_fg) gradient_free(&weaken_fg);
        if (has_weaken_bg) gradient_free(&weaken_bg);
        if (has_flash_fg) gradient_free(&flash_fg);
        if (has_flash_bg) gradient_free(&flash_bg);
        if (has_str_fg) gradient_free(&str_fg);
        if (has_str_bg) gradient_free(&str_bg);
    }

    free(characters);
    coordcolormap_free(&final_gradient_mapping);
    gradient_free(&final_gradient);
    if (rc != 0) {
        return -1;
    }

    rng_shuffle(&ctx->rng, st->pending.items, st->pending.len, sizeof(CharId));
    st->fall_delay = 12;
    st->max_fall_delay = 12;
    st->min_fall_delay = 9;
    st->reset = false;
    st->fall_group_maxsize = 1;
    st->stage = CRUMBLE_FALLING;
    for (size_t i = 0; i < ctx->terminal.input_characters_len; i++) {
        idvec_push(&st->unvacuumed, ctx->terminal.input_characters[i]);
    }
    rng_shuffle(&ctx->rng, st->unvacuumed.items, st->unvacuumed.len, sizeof(CharId));
    return 0;
}

static char *crumble_next_frame(Effect *self, EngineCtx *ctx) {
    Crumble *st = self->state;
    if (st->stage != CRUMBLE_COMPLETE) {
        switch (st->stage) {
            case CRUMBLE_FALLING:
                if (st->pending.len > 0) {
                    if (st->fall_delay == 0) {
                        int64_t fall_group_size = rng_randint(&ctx->rng, 1, st->fall_group_maxsize);
                        for (int64_t i = 0; i < fall_group_size; i++) {
                            if (st->pending.len > 0) {
                                CharId next_char = idvec_remove0(&st->pending);
                                engine_activate_scene(ctx, self, next_char, "weaken");
                                ac_insert(&ctx->active_characters, next_char);
                            }
                        }
                        st->fall_delay = rng_randint(&ctx->rng, st->min_fall_delay, st->max_fall_delay);
                        if (rng_randint(&ctx->rng, 1, 10) > 4) {
                            st->fall_group_maxsize += 1;
                            st->min_fall_delay = st->min_fall_delay - 1 > 0 ? st->min_fall_delay - 1 : 0;
                            st->max_fall_delay = st->max_fall_delay - 1 > 0 ? st->max_fall_delay - 1 : 0;
                        }
                    } else {
                        st->fall_delay -= 1;
                    }
                }
                if (st->pending.len == 0 && ac_is_empty(&ctx->active_characters)) {
                    st->stage = CRUMBLE_VACUUMING;
                }
                break;
            case CRUMBLE_VACUUMING:
                if (st->unvacuumed.len > 0) {
                    int64_t count = rng_randint(&ctx->rng, 3, 10);
                    for (int64_t i = 0; i < count; i++) {
                        if (st->unvacuumed.len > 0) {
                            CharId next_char = idvec_remove0(&st->unvacuumed);
                            engine_activate_path(ctx, self, next_char, "top");
                            ac_insert(&ctx->active_characters, next_char);
                        }
                    }
                }
                if (ac_is_empty(&ctx->active_characters)) {
                    st->stage = CRUMBLE_RESETTING;
                }
                break;
            case CRUMBLE_RESETTING: {
                if (!st->reset) {
                    size_t n = 0;
                    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng,
                                                                 character_filter_default(),
                                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);
                    for (size_t i = 0; i < n; i++) {
                        engine_activate_path(ctx, self, characters[i], "input");
                        ac_insert(&ctx->active_characters, characters[i]);
                    }
                    free(characters);
                    st->reset = true;
                }
                if (ac_is_empty(&ctx->active_characters)) {
                    st->stage = CRUMBLE_COMPLETE;
                }
                break;
            }
            case CRUMBLE_COMPLETE:
                break;
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void crumble_destroy(Effect *self) {
    Crumble *st = self->state;
    if (!st) {
        return;
    }
    idvec_free(&st->pending);
    idvec_free(&st->unvacuumed);
    free(st->character_final_color_map);
    free(st->character_final_present);
    free(st);
    free(self);
}

static const EffectOps CRUMBLE_OPS = {crumble_build, crumble_next_frame, crumble_destroy, NULL};

Effect *crumble_make(const void *cfg) {
    Crumble *st = calloc(1, sizeof(Crumble));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const CrumbleConfig *)cfg;
    effect->ops = &CRUMBLE_OPS;
    effect->state = st;
    return effect;
}

const EffectEntry crumble_entry = {
    "crumble",
    (const EffOptSpec[]){
        EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(CrumbleConfig, final_gradient_stops)),
        EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(CrumbleConfig, final_gradient_steps)),
        EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(CrumbleConfig, final_gradient_direction)),
    },
    3,
    sizeof(CrumbleConfig),
    crumble_config_defaults,
    crumble_free_config,
    crumble_make,
};
