#include "effects/pour.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/graphics.h"

typedef struct {
    CharId *items;
    size_t len;
} PourGroup;

typedef struct {
    PourConfig config;
    PourGroup *pending;
    size_t pending_len;
    size_t pending_cap;
    size_t pending_head;
    PourGroup current;
    size_t current_index;
    int64_t gap;
} Pour;

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

static int parse_pour_direction(const char *value, void *dst) {
    PourDirection *d = dst;
    if (strcmp(value, "up") == 0) {
        *d = POUR_UP;
    } else if (strcmp(value, "down") == 0) {
        *d = POUR_DOWN;
    } else if (strcmp(value, "left") == 0) {
        *d = POUR_LEFT;
    } else if (strcmp(value, "right") == 0) {
        *d = POUR_RIGHT;
    } else {
        return -1;
    }
    return 0;
}

void pour_config_defaults(void *cfg_ptr) {
    PourConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->pour_direction = POUR_DOWN;
    cfg->pour_speed = 2;
    cfg->movement_speed_range.start = 0.4;
    cfg->movement_speed_range.end = 0.6;
    cfg->gap = 1;
    color_from_hex("ffffff", &cfg->starting_color);
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "FFFFFF");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_frames = 6;
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
    easing_parse("in_quad", &cfg->movement_easing);
}

void pour_free_config(void *cfg_ptr) {
    PourConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static void pour_push_group(Pour *st, const CharId *items, size_t len, bool reverse) {
    if (st->pending_len == st->pending_cap) {
        size_t cap = st->pending_cap ? st->pending_cap * 2 : 8;
        PourGroup *grown = realloc(st->pending, cap * sizeof(PourGroup));
        if (!grown) {
            return;
        }
        st->pending = grown;
        st->pending_cap = cap;
    }
    CharId *copy = malloc((len ? len : 1) * sizeof(CharId));
    for (size_t i = 0; i < len; i++) {
        copy[i] = reverse ? items[len - 1 - i] : items[i];
    }
    st->pending[st->pending_len].items = copy;
    st->pending[st->pending_len].len = len;
    st->pending_len++;
}

// pending_groups.remove(0): transfer ownership of the head group to current.
static void pour_load_next(Pour *st) {
    if (st->pending_head >= st->pending_len) {
        return;
    }
    st->current = st->pending[st->pending_head];
    st->pending[st->pending_head].items = NULL;
    st->pending[st->pending_head].len = 0;
    st->pending_head++;
    st->current_index = 0;
}

static int pour_build(Effect *self, EngineCtx *ctx) {
    Pour *st = self->state;
    PourConfig *cfg = &st->config;
    PourDirection pour_direction = cfg->pour_direction;

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
    free(characters);

    CharacterGroup grouping;
    switch (pour_direction) {
        case POUR_DOWN:
            grouping = CG_ROW_BOTTOM_TO_TOP;
            break;
        case POUR_UP:
            grouping = CG_ROW_TOP_TO_BOTTOM;
            break;
        case POUR_LEFT:
            grouping = CG_COLUMN_LEFT_TO_RIGHT;
            break;
        case POUR_RIGHT:
        default:
            grouping = CG_COLUMN_RIGHT_TO_LEFT;
            break;
    }
    CharIdGrouping groups = terminal_get_characters_grouped(&ctx->terminal, character_filter_default(), grouping);

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    int rc = 0;
    for (size_t gi = 0; gi < groups.len && rc == 0; gi++) {
        CharIdBucket *group = &groups.buckets[gi];
        for (size_t k = 0; k < group->len && rc == 0; k++) {
            CharId id = group->items[k];
            terminal_set_character_visibility(&ctx->terminal, id, false);
            EffectCharacter *ch = &ctx->terminal.arena.items[id];
            Coord input_coord = ch->input_coord;
            char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
            strcpy(input_symbol, ch->input_symbol);
            bool uses_pre = ch->uses_input_preexisting_colors;

            Coord start_coord;
            switch (pour_direction) {
                case POUR_DOWN:
                    start_coord = coord_new(input_coord.column, ctx->terminal.canvas.top);
                    break;
                case POUR_UP:
                    start_coord = coord_new(input_coord.column, ctx->terminal.canvas.bottom);
                    break;
                case POUR_LEFT:
                    start_coord = coord_new(ctx->terminal.canvas.right, input_coord.row);
                    break;
                case POUR_RIGHT:
                default:
                    start_coord = coord_new(ctx->terminal.canvas.left, input_coord.row);
                    break;
            }
            motion_set_coordinate(&ctx->terminal.arena.items[id].motion, start_coord);
            double speed =
                rng_uniform(&ctx->rng, cfg->movement_speed_range.start, cfg->movement_speed_range.end);
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
            engine_activate_path(ctx, self, id, path_id);
            free(path_id);

            ch = &ctx->terminal.arena.items[id];
            const char *scene_id =
                animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "", uses_pre);
            Scene *scene = (Scene *)om_get(&ch->animation.scenes, scene_id);

            ColorPair final_colors;
            memset(&final_colors, 0, sizeof(final_colors));
            if (dynamic) {
                final_colors.has_fg = ch->animation.has_input_fg;
                final_colors.fg = ch->animation.input_fg_color;
                final_colors.has_bg = ch->animation.has_input_bg;
                final_colors.bg = ch->animation.input_bg_color;
            } else {
                const Color *cc = coordcolormap_get(&mapping, input_coord);
                final_colors.has_fg = true;
                if (cc) {
                    final_colors.fg = *cc;
                }
            }

            const char *symbols[1] = {input_symbol};
            if (dynamic) {
                if (final_colors.has_fg || final_colors.has_bg) {
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
                    if (rc == 0 &&
                        scene_apply_gradient_to_symbols(scene, symbols, 1, cfg->final_gradient_frames,
                                                        has_fg_grad ? &fg_grad : NULL,
                                                        has_bg_grad ? &bg_grad : NULL) != 0) {
                        rc = -1;
                    }
                    if (has_fg_grad) {
                        gradient_free(&fg_grad);
                    }
                    if (has_bg_grad) {
                        gradient_free(&bg_grad);
                    }
                } else {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    if (scene_add_frame(scene, input_symbol, cfg->final_gradient_frames, &vp) != 0) {
                        rc = -1;
                    }
                }
            } else {
                Color stops[2] = {cfg->starting_color, final_colors.fg};
                Gradient gradient;
                if (gradient_new(stops, 2, cfg->final_gradient_steps.items, cfg->final_gradient_steps.len, false,
                                 false, &gradient) != 0) {
                    rc = -1;
                } else {
                    if (scene_apply_gradient_to_symbols(scene, symbols, 1, cfg->final_gradient_frames, &gradient,
                                                        NULL) != 0) {
                        rc = -1;
                    }
                    gradient_free(&gradient);
                }
            }
            if (rc == 0) {
                engine_activate_scene(ctx, self, id, scene_id);
            }
            free(input_symbol);
        }
        if (rc != 0) {
            break;
        }
        pour_push_group(st, group->items, group->len, (gi % 2) != 0);
    }

    charidgrouping_free(&groups);
    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);

    st->gap = 0;
    pour_load_next(st);
    return rc;
}

static char *pour_next_frame(Effect *self, EngineCtx *ctx) {
    Pour *st = self->state;
    bool pending_remaining = st->pending_head < st->pending_len;
    size_t current_remaining = st->current.len - st->current_index;
    if (pending_remaining || !ac_is_empty(&ctx->active_characters) || current_remaining > 0) {
        if (current_remaining == 0 && pending_remaining) {
            pour_load_next(st);
        }
        current_remaining = st->current.len - st->current_index;
        if (current_remaining > 0) {
            if (st->gap == 0) {
                for (int64_t i = 0; i < st->config.pour_speed; i++) {
                    if (st->current_index < st->current.len) {
                        CharId next = st->current.items[st->current_index++];
                        terminal_set_character_visibility(&ctx->terminal, next, true);
                        ac_insert(&ctx->active_characters, next);
                    }
                }
                st->gap = st->config.gap;
            } else {
                st->gap -= 1;
            }
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void pour_destroy(Effect *self) {
    Pour *st = self->state;
    if (!st) {
        return;
    }
    for (size_t i = 0; i < st->pending_len; i++) {
        free(st->pending[i].items);
    }
    free(st->pending);
    free(st->current.items);
    free(st);
    free(self);
}

static const EffectOps POUR_OPS = {pour_build, pour_next_frame, pour_destroy, NULL};

Effect *pour_make(const void *cfg) {
    Pour *st = calloc(1, sizeof(Pour));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const PourConfig *)cfg;
    effect->ops = &POUR_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec pour_specs[] = {
    {"pour-direction", 0, EF_CUSTOM, offsetof(PourConfig, pour_direction), parse_pour_direction},
    EF_SPEC("pour-speed", 0, EF_POS_INT, offsetof(PourConfig, pour_speed)),
    EF_SPEC("movement-speed-range", 0, EF_FLOAT_RANGE, offsetof(PourConfig, movement_speed_range)),
    EF_SPEC("gap", 0, EF_NONNEG_INT, offsetof(PourConfig, gap)),
    EF_SPEC("starting-color", 0, EF_COLOR, offsetof(PourConfig, starting_color)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(PourConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(PourConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-frames", 0, EF_INT, offsetof(PourConfig, final_gradient_frames)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(PourConfig, final_gradient_direction)),
    EF_SPEC("movement-easing", 0, EF_EASING, offsetof(PourConfig, movement_easing)),
};

const EffectEntry pour_entry = {
    "pour",
    pour_specs,
    sizeof(pour_specs) / sizeof(pour_specs[0]),
    sizeof(PourConfig),
    pour_config_defaults,
    pour_free_config,
    pour_make,
};
