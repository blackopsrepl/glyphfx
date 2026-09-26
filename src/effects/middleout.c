#include "effects/middleout.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/graphics.h"

typedef enum {
    MIDDLEOUT_PHASE_CENTER,
    MIDDLEOUT_PHASE_FULL,
} MiddleoutPhase;

typedef struct {
    MiddleoutConfig config;
    MiddleoutPhase phase;
} Middleout;

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

static int parse_middleout_direction(const char *value, void *dst) {
    MiddleoutExpandDirection *d = dst;
    if (strcmp(value, "vertical") == 0) {
        *d = MIDDLEOUT_VERTICAL;
    } else if (strcmp(value, "horizontal") == 0) {
        *d = MIDDLEOUT_HORIZONTAL;
    } else {
        return -1;
    }
    return 0;
}

void middleout_config_defaults(void *cfg_ptr) {
    MiddleoutConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    color_from_hex("ffffff", &cfg->starting_color);
    cfg->expand_direction = MIDDLEOUT_VERTICAL;
    cfg->center_movement_speed = 0.6;
    cfg->full_movement_speed = 0.6;
    easing_parse("in_out_sine", &cfg->center_easing);
    easing_parse("in_out_sine", &cfg->full_easing);
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "FFFFFF");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void middleout_free_config(void *cfg_ptr) {
    MiddleoutConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static ColorPair middleout_final_colors(EngineCtx *ctx, CoordColorMap *mapping, bool dynamic, CharId id) {
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

static int middleout_build(Effect *self, EngineCtx *ctx) {
    Middleout *st = self->state;
    MiddleoutConfig *cfg = &st->config;

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

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        Coord input_coord = ch->input_coord;
        bool uses_pre = ch->uses_input_preexisting_colors;
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);

        ColorPair final_colors = middleout_final_colors(ctx, &mapping, dynamic, id);

        Coord center = ctx->terminal.canvas.center;
        motion_set_coordinate(&ctx->terminal.arena.items[id].motion, center);

        int64_t column;
        int64_t row;
        if (cfg->expand_direction == MIDDLEOUT_VERTICAL) {
            column = input_coord.column;
            row = ctx->terminal.canvas.center_row;
        } else {
            column = ctx->terminal.canvas.center_column;
            row = input_coord.row;
        }

        char *center_path_id = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, cfg->center_movement_speed, true,
                            cfg->center_easing, false, 0, 0, false, "", &center_path_id) != 0) {
            free(input_symbol);
            rc = -1;
            break;
        }
        Path *center_path = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, center_path_id);
        Waypoint wp;
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(center_path, coord_new(column, row), NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(center_path_id);
            free(input_symbol);
            rc = -1;
            break;
        }
        waypoint_free(&wp);

        char *full_path_id = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, cfg->full_movement_speed, true,
                            cfg->full_easing, false, 0, 0, false, "full", &full_path_id) != 0) {
            free(center_path_id);
            free(input_symbol);
            rc = -1;
            break;
        }
        Path *full_path = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, full_path_id);
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(full_path, input_coord, NULL, 0, "full", &wp) != 0) {
            waypoint_free(&wp);
            free(full_path_id);
            free(center_path_id);
            free(input_symbol);
            rc = -1;
            break;
        }
        waypoint_free(&wp);
        free(full_path_id);

        ch = &ctx->terminal.arena.items[id];
        const char *scene_id =
            animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "full", uses_pre);
        Scene *scene = (Scene *)om_get(&ch->animation.scenes, scene_id);
        const char *symbols[1] = {input_symbol};

        if (dynamic) {
            Gradient fg_grad;
            Gradient bg_grad;
            bool has_fg_grad = false;
            bool has_bg_grad = false;
            if (final_colors.has_fg) {
                Color stops[2] = {cfg->starting_color, final_colors.fg};
                if (gradient_with_steps(stops, 2, 10, false, &fg_grad) == 0) {
                    has_fg_grad = true;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0 && final_colors.has_bg) {
                Color stops[2] = {cfg->starting_color, final_colors.bg};
                if (gradient_with_steps(stops, 2, 10, false, &bg_grad) == 0) {
                    has_bg_grad = true;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0 && (has_fg_grad || has_bg_grad)) {
                if (scene_apply_gradient_to_symbols(scene, symbols, 1, 6, has_fg_grad ? &fg_grad : NULL,
                                                    has_bg_grad ? &bg_grad : NULL) != 0) {
                    rc = -1;
                }
            } else if (rc == 0) {
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                if (scene_add_frame(scene, input_symbol, 6, &vp) != 0) {
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
            Color stops[2] = {cfg->starting_color, final_colors.fg};
            Gradient full_gradient;
            if (gradient_with_steps(stops, 2, 10, false, &full_gradient) != 0) {
                rc = -1;
            } else {
                if (scene_apply_gradient_to_symbols(scene, symbols, 1, 6, &full_gradient, NULL) != 0) {
                    rc = -1;
                }
                gradient_free(&full_gradient);
            }
        }

        if (rc == 0) {
            engine_activate_path(ctx, self, id, center_path_id);

            ch = &ctx->terminal.arena.items[id];
            ColorPair start_colors;
            memset(&start_colors, 0, sizeof(start_colors));
            start_colors.has_fg = true;
            start_colors.fg = cfg->starting_color;
            animation_set_appearance(&ch->animation, uses_pre, ch->input_symbol, &start_colors);
            terminal_set_character_visibility(&ctx->terminal, id, true);
            ac_insert(&ctx->active_characters, id);
        }

        free(center_path_id);
        free(input_symbol);
    }

    free(characters);
    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    return rc;
}

static char *middleout_next_frame(Effect *self, EngineCtx *ctx) {
    Middleout *st = self->state;
    if (st->phase == MIDDLEOUT_PHASE_CENTER && ac_is_empty(&ctx->active_characters)) {
        st->phase = MIDDLEOUT_PHASE_FULL;
        size_t n = 0;
        CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                     CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);
        ac_clear(&ctx->active_characters);
        ac_extend(&ctx->active_characters, characters, n);
        free(characters);

        CharId *ordered = NULL;
        size_t ordered_len = 0;
        ac_snapshot(&ctx->active_characters, &ordered, &ordered_len);
        for (size_t i = 0; i < ordered_len; i++) {
            CharId id = ordered[i];
            engine_activate_path(ctx, self, id, "full");
            engine_activate_scene(ctx, self, id, "full");
        }
        free(ordered);
    }
    if (!ac_is_empty(&ctx->active_characters)) {
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void middleout_destroy(Effect *self) {
    Middleout *st = self->state;
    if (!st) {
        return;
    }
    free(st);
    free(self);
}

static const EffectOps MIDDLEOUT_OPS = {middleout_build, middleout_next_frame, middleout_destroy, NULL};

Effect *middleout_make(const void *cfg) {
    Middleout *st = calloc(1, sizeof(Middleout));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const MiddleoutConfig *)cfg;
    st->phase = MIDDLEOUT_PHASE_CENTER;
    effect->ops = &MIDDLEOUT_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec middleout_specs[] = {
    EF_SPEC("starting-color", 0, EF_COLOR, offsetof(MiddleoutConfig, starting_color)),
    {"expand-direction", 0, EF_CUSTOM, offsetof(MiddleoutConfig, expand_direction), parse_middleout_direction},
    EF_SPEC("center-movement-speed", 0, EF_FLOAT_POS, offsetof(MiddleoutConfig, center_movement_speed)),
    EF_SPEC("full-movement-speed", 0, EF_FLOAT_POS, offsetof(MiddleoutConfig, full_movement_speed)),
    EF_SPEC("center-easing", 0, EF_EASING, offsetof(MiddleoutConfig, center_easing)),
    EF_SPEC("full-easing", 0, EF_EASING, offsetof(MiddleoutConfig, full_easing)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(MiddleoutConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(MiddleoutConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(MiddleoutConfig, final_gradient_direction)),
};

const EffectEntry middleout_entry = {
    "middleout",
    middleout_specs,
    sizeof(middleout_specs) / sizeof(middleout_specs[0]),
    sizeof(MiddleoutConfig),
    middleout_config_defaults,
    middleout_free_config,
    middleout_make,
};
