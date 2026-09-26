#include "effects/rain.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/graphics.h"
#include "utils/utf8.h"

typedef struct {
    int64_t row;
    CharId *items;
    size_t len;
} RainRowGroup;

typedef struct {
    RainConfig config;
    CharId *pending;
    size_t pending_len;
    RainRowGroup *groups;
    size_t groups_len;
    size_t groups_head;
} Rain;

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

static void push_symbol_default(RainSymbolList *list, const char *symbol) {
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

void rain_config_defaults(void *cfg_ptr) {
    RainConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    push_color_default(&cfg->rain_colors, "00315C");
    push_color_default(&cfg->rain_colors, "004C8F");
    push_color_default(&cfg->rain_colors, "0075DB");
    push_color_default(&cfg->rain_colors, "3F91D9");
    push_color_default(&cfg->rain_colors, "78B9F2");
    push_color_default(&cfg->rain_colors, "9AC8F5");
    push_color_default(&cfg->rain_colors, "B8D8F8");
    push_color_default(&cfg->rain_colors, "E3EFFC");
    cfg->movement_speed.start = 0.33;
    cfg->movement_speed.end = 0.57;
    push_symbol_default(&cfg->rain_symbols, "o");
    push_symbol_default(&cfg->rain_symbols, ".");
    push_symbol_default(&cfg->rain_symbols, ",");
    push_symbol_default(&cfg->rain_symbols, "*");
    push_symbol_default(&cfg->rain_symbols, "|");
    push_color_default(&cfg->final_gradient_stops, "488bff");
    push_color_default(&cfg->final_gradient_stops, "b2e7de");
    push_color_default(&cfg->final_gradient_stops, "57eaf7");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_DIAGONAL;
    easing_parse("in_quart", &cfg->movement_easing);
}

void rain_free_config(void *cfg_ptr) {
    RainConfig *cfg = cfg_ptr;
    free(cfg->rain_colors.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
    for (size_t i = 0; i < cfg->rain_symbols.len; i++) {
        free(cfg->rain_symbols.items[i]);
    }
    free(cfg->rain_symbols.items);
}

static ColorPair rain_final_colors(EngineCtx *ctx, CoordColorMap *mapping, bool dynamic, CharId id) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    ColorPair fc;
    memset(&fc, 0, sizeof(fc));
    if (dynamic) {
        fc.has_fg = ch->animation.has_input_fg;
        fc.fg = ch->animation.input_fg_color;
        fc.has_bg = ch->animation.has_input_bg;
        fc.bg = ch->animation.input_bg_color;
    } else {
        const Color *cc = coordcolormap_get(mapping, ch->input_coord);
        fc.has_fg = true;
        if (cc) {
            fc.fg = *cc;
        }
    }
    return fc;
}

static void rain_stable_sort_by_row(CharId *ids, size_t n, const Arena *arena) {
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

static int rain_build(Effect *self, EngineCtx *ctx) {
    Rain *st = self->state;
    RainConfig *cfg = &st->config;

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
        bool uses_pre = ch->uses_input_preexisting_colors;
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);

        Color raindrop_color = cfg->rain_colors.items[rng_choice_index(&ctx->rng, cfg->rain_colors.len)];

        ch = &ctx->terminal.arena.items[id];
        const char *rain_scene_id = animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease,
                                                        "", uses_pre);
        Scene *rain_scene = (Scene *)om_get(&ch->animation.scenes, rain_scene_id);

        const char *rain_symbol = cfg->rain_symbols.items[rng_choice_index(&ctx->rng, cfg->rain_symbols.len)];
        VisualParams rain_vp;
        memset(&rain_vp, 0, sizeof(rain_vp));
        rain_vp.has_colors = true;
        rain_vp.colors.has_fg = true;
        rain_vp.colors.fg = raindrop_color;
        if (scene_add_frame(rain_scene, rain_symbol, 1, &rain_vp) != 0) {
            free(input_symbol);
            rc = -1;
            break;
        }

        ch = &ctx->terminal.arena.items[id];
        const char *fade_scene_id = animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease,
                                                        "", uses_pre);
        Scene *fade_scene = (Scene *)om_get(&ch->animation.scenes, fade_scene_id);
        ColorPair final_colors = rain_final_colors(ctx, &mapping, dynamic, id);
        const char *symbols[1] = {input_symbol};

        if (dynamic) {
            Gradient fg_grad;
            Gradient bg_grad;
            bool has_fg_grad = false;
            bool has_bg_grad = false;
            if (final_colors.has_fg) {
                Color stops[2] = {raindrop_color, final_colors.fg};
                if (gradient_with_steps(stops, 2, 7, false, &fg_grad) == 0) {
                    has_fg_grad = true;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0 && final_colors.has_bg) {
                Color stops[2] = {raindrop_color, final_colors.bg};
                if (gradient_with_steps(stops, 2, 7, false, &bg_grad) == 0) {
                    has_bg_grad = true;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0 && (has_fg_grad || has_bg_grad)) {
                if (scene_apply_gradient_to_symbols(fade_scene, symbols, 1, 3, has_fg_grad ? &fg_grad : NULL,
                                                    has_bg_grad ? &bg_grad : NULL) != 0) {
                    rc = -1;
                }
            } else if (rc == 0) {
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                if (scene_add_frame(fade_scene, input_symbol, 3, &vp) != 0) {
                    rc = -1;
                }
            }
            if (has_fg_grad) {
                gradient_free(&fg_grad);
            }
            if (has_bg_grad) {
                gradient_free(&bg_grad);
            }
        } else {
            Color stops[2] = {raindrop_color, final_colors.fg};
            Gradient raindrop_gradient;
            if (gradient_with_steps(stops, 2, 7, false, &raindrop_gradient) != 0) {
                rc = -1;
            } else {
                if (scene_apply_gradient_to_symbols(fade_scene, symbols, 1, 3, &raindrop_gradient, NULL) != 0) {
                    rc = -1;
                }
                gradient_free(&raindrop_gradient);
            }
        }

        if (rc == 0) {
            engine_activate_scene(ctx, self, id, rain_scene_id);

            double speed = rng_uniform(&ctx->rng, cfg->movement_speed.start, cfg->movement_speed.end);
            motion_set_coordinate(&ctx->terminal.arena.items[id].motion, coord_new(input_coord.column, canvas_top));
            char *path_id = NULL;
            if (motion_new_path(&ctx->terminal.arena.items[id].motion, speed, true, cfg->movement_easing, false, 0,
                                0, false, "", &path_id) != 0) {
                free(input_symbol);
                rc = -1;
                break;
            }
            Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, path_id);
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

            CallerKey caller;
            memset(&caller, 0, sizeof(caller));
            caller.kind = CALLER_PATH;
            caller.id = path_id;
            EventAction activate_fade;
            memset(&activate_fade, 0, sizeof(activate_fade));
            activate_fade.kind = ACTION_ACTIVATE_SCENE;
            activate_fade.id = (char *)fade_scene_id;
            if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &activate_fade) != 0) {
                free(path_id);
                free(input_symbol);
                rc = -1;
                break;
            }

            engine_activate_path(ctx, self, id, path_id);
            free(path_id);
            st->pending[pending_count++] = id;
        }
        free(input_symbol);
    }

    st->pending_len = pending_count;

    if (rc == 0) {
        CharId *sorted = malloc((pending_count ? pending_count : 1) * sizeof(CharId));
        for (size_t i = 0; i < pending_count; i++) {
            sorted[i] = st->pending[i];
        }
        rain_stable_sort_by_row(sorted, pending_count, &ctx->terminal.arena);

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
                st->groups = realloc(st->groups, group_cap * sizeof(RainRowGroup));
            }
            RainRowGroup *group = &st->groups[st->groups_len++];
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
    return rc;
}

static const char *rain_next_frame(Effect *self, EngineCtx *ctx) {
    Rain *st = self->state;
    if (st->groups_head < st->groups_len || !ac_is_empty(&ctx->active_characters) || st->pending_len > 0) {
        if (st->pending_len == 0 && st->groups_head < st->groups_len) {
            RainRowGroup *group = &st->groups[st->groups_head++];
            for (size_t i = 0; i < group->len; i++) {
                st->pending[i] = group->items[i];
            }
            st->pending_len = group->len;
        }
        if (st->pending_len > 0) {
            int64_t count = rng_randint(&ctx->rng, 1, 2);
            for (int64_t i = 0; i < count; i++) {
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
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void rain_destroy(Effect *self) {
    Rain *st = self->state;
    if (!st) {
        return;
    }
    free(st->pending);
    for (size_t i = st->groups_head; i < st->groups_len; i++) {
        free(st->groups[i].items);
    }
    free(st->groups);
    free(st);
    free(self);
}

static const EffectOps RAIN_OPS = {rain_build, rain_next_frame, rain_destroy, NULL};

Effect *rain_make(const void *cfg) {
    Rain *st = calloc(1, sizeof(Rain));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const RainConfig *)cfg;
    effect->ops = &RAIN_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec rain_specs[] = {
    EF_SPEC("rain-colors", 0, EF_COLOR_LIST, offsetof(RainConfig, rain_colors)),
    EF_SPEC("movement-speed", 0, EF_FLOAT_RANGE, offsetof(RainConfig, movement_speed)),
    {"rain-symbols", 0, EF_STRING_LIST, offsetof(RainConfig, rain_symbols), NULL},
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(RainConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(RainConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(RainConfig, final_gradient_direction)),
    EF_SPEC("movement-easing", 0, EF_EASING, offsetof(RainConfig, movement_easing)),
};

const EffectEntry rain_entry = {
    "rain",
    rain_specs,
    sizeof(rain_specs) / sizeof(rain_specs[0]),
    sizeof(RainConfig),
    rain_config_defaults,
    rain_free_config,
    rain_make,
};
