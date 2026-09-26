#include "effects/bouncyballs.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/graphics.h"

typedef struct {
    int64_t row;
    CharId *items;
    size_t len;
} BouncyBallRowGroup;

typedef struct {
    BouncyBallsConfig config;
    CharId *pending;
    size_t pending_len;
    BouncyBallRowGroup *groups;
    size_t groups_len;
    size_t groups_head;
    // Dense CharId -> final gradient color (the reference HashMap is only ever
    // read by key, never iterated, so id indexing is behaviorally identical).
    Color *final_colors;
    bool *final_colors_present;
    size_t final_colors_len;
    int64_t ball_delay;
} BouncyBalls;

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

static void push_symbol_default(BouncyBallsSymbolList *list, const char *symbol) {
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

void bouncyballs_config_defaults(void *cfg_ptr) {
    BouncyBallsConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    push_color_default(&cfg->ball_colors, "d1f4a5");
    push_color_default(&cfg->ball_colors, "96e2a4");
    push_color_default(&cfg->ball_colors, "5acda9");
    push_symbol_default(&cfg->ball_symbols, "*");
    push_symbol_default(&cfg->ball_symbols, "o");
    push_symbol_default(&cfg->ball_symbols, "O");
    push_symbol_default(&cfg->ball_symbols, "0");
    push_symbol_default(&cfg->ball_symbols, ".");
    cfg->ball_delay = 4;
    cfg->movement_speed = 0.45;
    easing_parse("out_bounce", &cfg->movement_easing);
    push_color_default(&cfg->final_gradient_stops, "f8ffae");
    push_color_default(&cfg->final_gradient_stops, "43c6ac");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_DIAGONAL;
}

void bouncyballs_free_config(void *cfg_ptr) {
    BouncyBallsConfig *cfg = cfg_ptr;
    free(cfg->ball_colors.items);
    for (size_t i = 0; i < cfg->ball_symbols.len; i++) {
        free(cfg->ball_symbols.items[i]);
    }
    free(cfg->ball_symbols.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static void bouncyballs_stable_sort_by_row(CharId *ids, size_t n, const Arena *arena) {
    for (size_t i = 1; i < n; i++) {
        CharId key = ids[i];
        int64_t key_row = arena->items[key].input_coord.row;
        size_t j = i;
        while (j > 0 && arena->items[ids[j - 1]].input_coord.row > key_row) {
            ids[j] = ids[j - 1];
            j--;
        }
        ids[j] = key;
    }
}

static int bouncyballs_build(Effect *self, EngineCtx *ctx) {
    BouncyBalls *st = self->state;
    BouncyBallsConfig *cfg = &st->config;

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
    st->final_colors_len = arena_len;
    st->final_colors = calloc(arena_len ? arena_len : 1, sizeof(Color));
    st->final_colors_present = calloc(arena_len ? arena_len : 1, sizeof(bool));
    st->pending = malloc((n ? n : 1) * sizeof(CharId));

    int64_t canvas_top = ctx->terminal.canvas.top;
    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    int rc = 0;
    size_t pending_count = 0;
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
        Color mapped_color;
        memset(&mapped_color, 0, sizeof(mapped_color));
        if (mapped) {
            mapped_color = *mapped;
        }
        if ((size_t)id < st->final_colors_len) {
            st->final_colors[id] = mapped_color;
            st->final_colors_present[id] = true;
        }

        Color color = cfg->ball_colors.items[rng_choice_index(&ctx->rng, cfg->ball_colors.len)];
        const char *symbol = cfg->ball_symbols.items[rng_choice_index(&ctx->rng, cfg->ball_symbols.len)];

        ch = &ctx->terminal.arena.items[id];
        const char *ball_scene_id = animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease,
                                                        "", uses_pre);
        Scene *ball_scene = (Scene *)om_get(&ch->animation.scenes, ball_scene_id);
        VisualParams ball_vp;
        memset(&ball_vp, 0, sizeof(ball_vp));
        ball_vp.has_colors = true;
        ball_vp.colors.has_fg = true;
        ball_vp.colors.fg = color;
        if (scene_add_frame(ball_scene, symbol, 1, &ball_vp) != 0) {
            free(input_symbol);
            rc = -1;
            break;
        }

        ch = &ctx->terminal.arena.items[id];
        const char *final_scene_id = animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease,
                                                         "", uses_pre);

        if (dynamic) {
            Gradient fg_grad;
            Gradient bg_grad;
            bool has_fg_grad = false;
            bool has_bg_grad = false;
            if (has_input_fg) {
                Color stops[2] = {color, input_fg};
                if (gradient_with_steps(stops, 2, 10, false, &fg_grad) == 0) {
                    has_fg_grad = true;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0 && has_input_bg) {
                Color stops[2] = {color, input_bg};
                if (gradient_with_steps(stops, 2, 10, false, &bg_grad) == 0) {
                    has_bg_grad = true;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0) {
                Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, final_scene_id);
                if (has_fg_grad || has_bg_grad) {
                    const char *symbols[1] = {input_symbol};
                    if (scene_apply_gradient_to_symbols(scene, symbols, 1, 6, has_fg_grad ? &fg_grad : NULL,
                                                        has_bg_grad ? &bg_grad : NULL) != 0) {
                        rc = -1;
                    }
                } else {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    if (scene_add_frame(scene, input_symbol, 6, &vp) != 0) {
                        rc = -1;
                    }
                }
            }
            if (has_fg_grad) {
                gradient_free(&fg_grad);
            }
            if (has_bg_grad) {
                gradient_free(&bg_grad);
            }
        } else {
            Color final_color = st->final_colors[id];
            Color stops[2] = {color, final_color};
            Gradient char_gradient;
            if (gradient_with_steps(stops, 2, 10, false, &char_gradient) != 0) {
                rc = -1;
            } else {
                Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, final_scene_id);
                const char *symbols[1] = {input_symbol};
                if (scene_apply_gradient_to_symbols(scene, symbols, 1, 6, &char_gradient, NULL) != 0) {
                    rc = -1;
                }
                gradient_free(&char_gradient);
            }
        }
        if (rc != 0) {
            free(input_symbol);
            break;
        }

        int64_t drop_row = (int64_t)((double)canvas_top * rng_uniform(&ctx->rng, 1.0, 1.5));
        ch = &ctx->terminal.arena.items[id];
        motion_set_coordinate(&ch->motion, coord_new(input_coord.column, drop_row));
        char *path_id = NULL;
        if (motion_new_path(&ch->motion, cfg->movement_speed, true, cfg->movement_easing, false, 0, 0, false, "",
                            &path_id) != 0) {
            free(input_symbol);
            rc = -1;
            break;
        }
        Path *p = (Path *)om_get(&ch->motion.paths, path_id);
        Waypoint wp;
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(p, input_coord, NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(path_id);
            free(input_symbol);
            rc = -1;
            break;
        }
        waypoint_free(&wp);

        engine_activate_path(ctx, self, id, path_id);
        engine_activate_scene(ctx, self, id, ball_scene_id);

        CallerKey caller;
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = path_id;
        EventAction activate_final;
        memset(&activate_final, 0, sizeof(activate_final));
        activate_final.kind = ACTION_ACTIVATE_SCENE;
        activate_final.id = (char *)final_scene_id;
        if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &activate_final) != 0) {
            free(path_id);
            free(input_symbol);
            rc = -1;
            break;
        }

        free(path_id);
        free(input_symbol);
        st->pending[pending_count++] = id;
    }

    st->pending_len = pending_count;

    if (rc == 0) {
        CharId *sorted = malloc((pending_count ? pending_count : 1) * sizeof(CharId));
        for (size_t i = 0; i < pending_count; i++) {
            sorted[i] = st->pending[i];
        }
        bouncyballs_stable_sort_by_row(sorted, pending_count, &ctx->terminal.arena);

        size_t group_cap = 0;
        size_t gi = 0;
        while (gi < pending_count) {
            int64_t row = ctx->terminal.arena.items[sorted[gi]].input_coord.row;
            size_t end = gi + 1;
            while (end < pending_count && ctx->terminal.arena.items[sorted[end]].input_coord.row == row) {
                end++;
            }
            if (st->groups_len == group_cap) {
                group_cap = group_cap ? group_cap * 2 : 8;
                st->groups = realloc(st->groups, group_cap * sizeof(BouncyBallRowGroup));
            }
            BouncyBallRowGroup *group = &st->groups[st->groups_len++];
            group->row = row;
            group->len = end - gi;
            group->items = malloc(group->len * sizeof(CharId));
            for (size_t k = 0; k < group->len; k++) {
                group->items[k] = sorted[gi + k];
            }
            gi = end;
        }
        free(sorted);
        st->pending_len = 0;
    }

    free(characters);
    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    st->ball_delay = 0;
    return rc;
}

static char *bouncyballs_next_frame(Effect *self, EngineCtx *ctx) {
    BouncyBalls *st = self->state;
    bool groups_remaining = st->groups_head < st->groups_len;
    if (groups_remaining || !ac_is_empty(&ctx->active_characters) || st->pending_len > 0) {
        if (st->pending_len == 0 && groups_remaining) {
            BouncyBallRowGroup *group = &st->groups[st->groups_head++];
            for (size_t i = 0; i < group->len; i++) {
                st->pending[i] = group->items[i];
            }
            st->pending_len = group->len;
        }
        if (st->pending_len > 0) {
            if (st->ball_delay == 0) {
                int64_t count = rng_randint(&ctx->rng, 2, 6);
                for (int64_t k = 0; k < count; k++) {
                    if (st->pending_len == 0) {
                        break;
                    }
                    int64_t index = rng_randint(&ctx->rng, 0, (int64_t)st->pending_len - 1);
                    CharId id = st->pending[index];
                    memmove(&st->pending[index], &st->pending[index + 1],
                            (st->pending_len - (size_t)index - 1) * sizeof(CharId));
                    st->pending_len--;
                    terminal_set_character_visibility(&ctx->terminal, id, true);
                    ac_insert(&ctx->active_characters, id);
                }
                st->ball_delay = st->config.ball_delay;
            } else {
                st->ball_delay -= 1;
            }
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void bouncyballs_destroy(Effect *self) {
    BouncyBalls *st = self->state;
    if (!st) {
        return;
    }
    free(st->pending);
    for (size_t i = 0; i < st->groups_len; i++) {
        free(st->groups[i].items);
    }
    free(st->groups);
    free(st->final_colors);
    free(st->final_colors_present);
    free(st);
    free(self);
}

static const EffectOps BOUNCYBALLS_OPS = {bouncyballs_build, bouncyballs_next_frame, bouncyballs_destroy, NULL};

Effect *bouncyballs_make(const void *cfg) {
    BouncyBalls *st = calloc(1, sizeof(BouncyBalls));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const BouncyBallsConfig *)cfg;
    effect->ops = &BOUNCYBALLS_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec bouncyballs_specs[] = {
    EF_SPEC("ball-colors", 0, EF_COLOR_LIST, offsetof(BouncyBallsConfig, ball_colors)),
    {"ball-symbols", 0, EF_STRING_LIST, offsetof(BouncyBallsConfig, ball_symbols), NULL},
    EF_SPEC("ball-delay", 0, EF_NONNEG_INT, offsetof(BouncyBallsConfig, ball_delay)),
    EF_SPEC("movement-speed", 0, EF_FLOAT_POS, offsetof(BouncyBallsConfig, movement_speed)),
    EF_SPEC("movement-easing", 0, EF_EASING, offsetof(BouncyBallsConfig, movement_easing)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(BouncyBallsConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(BouncyBallsConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(BouncyBallsConfig, final_gradient_direction)),
};

const EffectEntry bouncyballs_entry = {
    "bouncyballs",
    bouncyballs_specs,
    sizeof(bouncyballs_specs) / sizeof(bouncyballs_specs[0]),
    sizeof(BouncyBallsConfig),
    bouncyballs_config_defaults,
    bouncyballs_free_config,
    bouncyballs_make,
};
