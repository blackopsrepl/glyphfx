#include "effects/slide.h"

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
    size_t head;
} SlideGroup;

typedef struct {
    SlideConfig config;
    SlideGroup *pending;
    size_t pending_len;
    size_t pending_cap;
    size_t pending_head;
    SlideGroup *active;
    size_t active_len;
    size_t active_cap;
    int64_t current_gap;
} Slide;

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

static int parse_slide_grouping(const char *value, void *dst) {
    SlideGrouping *g = dst;
    if (strcmp(value, "row") == 0) {
        *g = SLIDE_GROUP_ROW;
    } else if (strcmp(value, "column") == 0) {
        *g = SLIDE_GROUP_COLUMN;
    } else if (strcmp(value, "diagonal") == 0) {
        *g = SLIDE_GROUP_DIAGONAL;
    } else {
        return -1;
    }
    return 0;
}

void slide_config_defaults(void *cfg_ptr) {
    SlideConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->movement_speed = 0.8;
    cfg->grouping = SLIDE_GROUP_ROW;
    cfg->gap = 2;
    cfg->reverse_direction = false;
    cfg->merge = false;
    easing_parse("in_out_quad", &cfg->movement_easing);
    push_color_default(&cfg->final_gradient_stops, "833ab4");
    push_color_default(&cfg->final_gradient_stops, "fd1d1d");
    push_color_default(&cfg->final_gradient_stops, "fcb045");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_frames = 6;
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void slide_free_config(void *cfg_ptr) {
    SlideConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static void reverse_ids(CharId *items, size_t len) {
    for (size_t i = 0; i < len / 2; i++) {
        CharId tmp = items[i];
        items[i] = items[len - 1 - i];
        items[len - 1 - i] = tmp;
    }
}

static bool slide_push_pending(Slide *st, SlideGroup group) {
    if (st->pending_len == st->pending_cap) {
        size_t cap = st->pending_cap ? st->pending_cap * 2 : 8;
        SlideGroup *grown = realloc(st->pending, cap * sizeof(SlideGroup));
        if (!grown) {
            return false;
        }
        st->pending = grown;
        st->pending_cap = cap;
    }
    st->pending[st->pending_len++] = group;
    return true;
}

static bool slide_push_active(Slide *st, SlideGroup group) {
    if (st->active_len == st->active_cap) {
        size_t cap = st->active_cap ? st->active_cap * 2 : 8;
        SlideGroup *grown = realloc(st->active, cap * sizeof(SlideGroup));
        if (!grown) {
            return false;
        }
        st->active = grown;
        st->active_cap = cap;
    }
    st->active[st->active_len++] = group;
    return true;
}

static ColorPair slide_final_colors(EngineCtx *ctx, CoordColorMap *mapping, bool dynamic, CharId id) {
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

static int slide_build(Effect *self, EngineCtx *ctx) {
    Slide *st = self->state;
    SlideConfig *cfg = &st->config;

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

    CharacterGroup grouping;
    switch (cfg->grouping) {
        case SLIDE_GROUP_ROW:
            grouping = CG_ROW_TOP_TO_BOTTOM;
            break;
        case SLIDE_GROUP_COLUMN:
            grouping = CG_COLUMN_LEFT_TO_RIGHT;
            break;
        case SLIDE_GROUP_DIAGONAL:
        default:
            grouping = CG_DIAGONAL_TOP_LEFT_TO_BOTTOM_RIGHT;
            break;
    }
    CharIdGrouping groups = terminal_get_characters_grouped(&ctx->terminal, character_filter_default(), grouping);

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    int rc = 0;

    // First pass: build the "input_path" for every character.
    for (size_t gi = 0; gi < groups.len && rc == 0; gi++) {
        CharIdBucket *bucket = &groups.buckets[gi];
        for (size_t k = 0; k < bucket->len && rc == 0; k++) {
            CharId id = bucket->items[k];
            Coord input_coord = ctx->terminal.arena.items[id].input_coord;
            char *path_id = NULL;
            if (motion_new_path(&ctx->terminal.arena.items[id].motion, cfg->movement_speed, true,
                                cfg->movement_easing, false, 0, 0, false, "input_path", &path_id) != 0) {
                rc = -1;
                break;
            }
            Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, path_id);
            Waypoint wp;
            memset(&wp, 0, sizeof(wp));
            if (path_new_waypoint(p, input_coord, NULL, 0, "", &wp) != 0) {
                waypoint_free(&wp);
                free(path_id);
                rc = -1;
                break;
            }
            waypoint_free(&wp);
            free(path_id);
        }
    }

    // Second pass: origins (may reverse a group in place) and scenes over the
    // original (pre-reversal) group order.
    for (size_t gi = 0; gi < groups.len && rc == 0; gi++) {
        CharIdBucket *bucket = &groups.buckets[gi];
        CharId *original = malloc((bucket->len ? bucket->len : 1) * sizeof(CharId));
        for (size_t k = 0; k < bucket->len; k++) {
            original[k] = bucket->items[k];
        }

        if (cfg->grouping == SLIDE_GROUP_ROW) {
            int64_t starting_column;
            if (cfg->merge && gi % 2 == 0) {
                starting_column = ctx->terminal.canvas.right + 1;
            } else {
                reverse_ids(bucket->items, bucket->len);
                starting_column = ctx->terminal.canvas.left - 1;
            }
            if (cfg->reverse_direction && !cfg->merge) {
                reverse_ids(bucket->items, bucket->len);
                starting_column = ctx->terminal.canvas.right + 1;
            }
            for (size_t k = 0; k < bucket->len; k++) {
                CharId id = bucket->items[k];
                int64_t row = ctx->terminal.arena.items[id].input_coord.row;
                motion_set_coordinate(&ctx->terminal.arena.items[id].motion, coord_new(starting_column, row));
            }
        } else if (cfg->grouping == SLIDE_GROUP_COLUMN) {
            int64_t starting_row;
            if (cfg->merge && gi % 2 == 0) {
                starting_row = ctx->terminal.canvas.bottom - 1;
            } else {
                reverse_ids(bucket->items, bucket->len);
                starting_row = ctx->terminal.canvas.top + 1;
            }
            if (cfg->reverse_direction && !cfg->merge) {
                reverse_ids(bucket->items, bucket->len);
                starting_row = ctx->terminal.canvas.bottom - 1;
            }
            for (size_t k = 0; k < bucket->len; k++) {
                CharId id = bucket->items[k];
                int64_t column = ctx->terminal.arena.items[id].input_coord.column;
                motion_set_coordinate(&ctx->terminal.arena.items[id].motion, coord_new(column, starting_row));
            }
        } else {
            Coord last_coord = ctx->terminal.arena.items[original[bucket->len - 1]].input_coord;
            int64_t distance_from_outside_bottom = last_coord.row - (ctx->terminal.canvas.bottom - 1);
            Coord starting_coord = coord_new(last_coord.column - distance_from_outside_bottom,
                                             last_coord.row - distance_from_outside_bottom);
            if (cfg->merge && gi % 2 == 0) {
                reverse_ids(bucket->items, bucket->len);
                Coord first_coord = ctx->terminal.arena.items[original[0]].input_coord;
                int64_t distance_from_outside = (ctx->terminal.canvas.top + 1) - first_coord.row;
                starting_coord = coord_new(first_coord.column + distance_from_outside,
                                           first_coord.row + distance_from_outside);
            }
            if (cfg->reverse_direction && !cfg->merge) {
                reverse_ids(bucket->items, bucket->len);
                Coord first_coord = ctx->terminal.arena.items[original[0]].input_coord;
                int64_t distance_from_outside = (ctx->terminal.canvas.top + 1) - first_coord.row;
                starting_coord = coord_new(first_coord.column + distance_from_outside,
                                           first_coord.row + distance_from_outside);
            }
            for (size_t k = 0; k < bucket->len; k++) {
                CharId id = bucket->items[k];
                motion_set_coordinate(&ctx->terminal.arena.items[id].motion, starting_coord);
            }
        }

        for (size_t k = 0; k < bucket->len && rc == 0; k++) {
            CharId id = original[k];
            EffectCharacter *ch = &ctx->terminal.arena.items[id];
            char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
            strcpy(input_symbol, ch->input_symbol);
            bool uses_pre = ch->uses_input_preexisting_colors;
            ColorPair final_colors = slide_final_colors(ctx, &mapping, dynamic, id);

            const char *scene_id = animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease,
                                                       "", uses_pre);
            Scene *scene = (Scene *)om_get(&ch->animation.scenes, scene_id);
            const char *symbols[1] = {input_symbol};
            if (dynamic) {
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                vp.colors = final_colors;
                if (scene_add_frame(scene, input_symbol, cfg->final_gradient_frames, &vp) != 0) {
                    rc = -1;
                }
            } else {
                Color stops[2] = {cfg->final_gradient_stops.items[0], final_colors.fg};
                Gradient char_gradient;
                if (gradient_with_steps(stops, 2, 10, false, &char_gradient) != 0) {
                    rc = -1;
                } else {
                    if (scene_apply_gradient_to_symbols(scene, symbols, 1, cfg->final_gradient_frames,
                                                        &char_gradient, NULL) != 0) {
                        rc = -1;
                    }
                    gradient_free(&char_gradient);
                }
            }
            if (rc == 0) {
                engine_activate_scene(ctx, self, id, scene_id);
            }
            free(input_symbol);
        }

        free(original);
    }

    // pending_groups = groups (in place reversal above is the stored order).
    for (size_t gi = 0; gi < groups.len; gi++) {
        SlideGroup group;
        group.items = groups.buckets[gi].items;
        group.len = groups.buckets[gi].len;
        group.head = 0;
        groups.buckets[gi].items = NULL;
        if (!slide_push_pending(st, group)) {
            free(group.items);
            rc = -1;
            break;
        }
    }

    charidgrouping_free(&groups);
    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);

    st->pending_head = 0;
    st->active_len = 0;
    st->current_gap = 0;
    return rc;
}

static void slide_retain_active(Slide *st) {
    size_t out = 0;
    for (size_t i = 0; i < st->active_len; i++) {
        SlideGroup *g = &st->active[i];
        if (g->head < g->len) {
            st->active[out++] = *g;
        } else {
            free(g->items);
        }
    }
    st->active_len = out;
}

static const char *slide_next_frame(Effect *self, EngineCtx *ctx) {
    Slide *st = self->state;
    bool pending_remaining = st->pending_head < st->pending_len;
    if (pending_remaining || !ac_is_empty(&ctx->active_characters) || st->active_len > 0) {
        if (st->current_gap == st->config.gap && pending_remaining) {
            SlideGroup group = st->pending[st->pending_head];
            st->pending[st->pending_head].items = NULL;
            st->pending[st->pending_head].len = 0;
            st->pending_head++;
            if (slide_push_active(st, group)) {
                st->current_gap = 0;
            } else {
                free(group.items);
            }
        } else if (pending_remaining) {
            st->current_gap += 1;
        }
        for (size_t gi = 0; gi < st->active_len; gi++) {
            SlideGroup *g = &st->active[gi];
            if (g->head < g->len) {
                CharId id = g->items[g->head++];
                terminal_set_character_visibility(&ctx->terminal, id, true);
                engine_activate_path(ctx, self, id, "input_path");
                ac_insert(&ctx->active_characters, id);
            }
        }
        slide_retain_active(st);
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void slide_destroy(Effect *self) {
    Slide *st = self->state;
    if (!st) {
        return;
    }
    for (size_t i = st->pending_head; i < st->pending_len; i++) {
        free(st->pending[i].items);
    }
    free(st->pending);
    for (size_t i = 0; i < st->active_len; i++) {
        free(st->active[i].items);
    }
    free(st->active);
    free(st);
    free(self);
}

static const EffectOps SLIDE_OPS = {slide_build, slide_next_frame, slide_destroy, NULL};

Effect *slide_make(const void *cfg) {
    Slide *st = calloc(1, sizeof(Slide));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const SlideConfig *)cfg;
    effect->ops = &SLIDE_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec slide_specs[] = {
    EF_SPEC("movement-speed", 0, EF_FLOAT_POS, offsetof(SlideConfig, movement_speed)),
    {"grouping", 0, EF_CUSTOM, offsetof(SlideConfig, grouping), parse_slide_grouping},
    EF_SPEC("gap", 0, EF_NONNEG_INT, offsetof(SlideConfig, gap)),
    EF_SPEC("reverse-direction", 0, EF_FLAG, offsetof(SlideConfig, reverse_direction)),
    EF_SPEC("merge", 0, EF_FLAG, offsetof(SlideConfig, merge)),
    EF_SPEC("movement-easing", 0, EF_EASING, offsetof(SlideConfig, movement_easing)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(SlideConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(SlideConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-frames", 0, EF_INT, offsetof(SlideConfig, final_gradient_frames)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(SlideConfig, final_gradient_direction)),
};

const EffectEntry slide_entry = {
    "slide",
    slide_specs,
    sizeof(slide_specs) / sizeof(slide_specs[0]),
    sizeof(SlideConfig),
    slide_config_defaults,
    slide_free_config,
    slide_make,
};
