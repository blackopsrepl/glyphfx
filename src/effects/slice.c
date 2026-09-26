#include "effects/slice.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/graphics.h"

typedef struct {
    SliceConfig config;
} Slice;

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

static int parse_slice_direction(const char *value, void *dst) {
    SliceDirection *d = dst;
    if (strcmp(value, "vertical") == 0) {
        *d = SLICE_VERTICAL;
    } else if (strcmp(value, "horizontal") == 0) {
        *d = SLICE_HORIZONTAL;
    } else if (strcmp(value, "diagonal") == 0) {
        *d = SLICE_DIAGONAL;
    } else {
        return -1;
    }
    return 0;
}

void slice_config_defaults(void *cfg_ptr) {
    SliceConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->slice_direction = SLICE_VERTICAL;
    cfg->movement_speed = 0.25;
    easing_parse("in_out_expo", &cfg->movement_easing);
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "FFFFFF");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_DIAGONAL;
}

void slice_free_config(void *cfg_ptr) {
    SliceConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

// send_to! macro equivalent: set the origin, build the input-coord path, and
// activate it. Re-fetches the character by id across every call.
static int slice_send_to(EngineCtx *ctx, Effect *self, SliceConfig *cfg, CharId id, Coord origin) {
    Coord input_coord = ctx->terminal.arena.items[id].input_coord;
    motion_set_coordinate(&ctx->terminal.arena.items[id].motion, origin);
    char *path_id = NULL;
    if (motion_new_path(&ctx->terminal.arena.items[id].motion, cfg->movement_speed, true, cfg->movement_easing,
                        false, 0, 0, false, "", &path_id) != 0) {
        return -1;
    }
    Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, path_id);
    Waypoint wp;
    memset(&wp, 0, sizeof(wp));
    if (path_new_waypoint(p, input_coord, NULL, 0, "", &wp) != 0) {
        waypoint_free(&wp);
        free(path_id);
        return -1;
    }
    waypoint_free(&wp);
    engine_activate_path(ctx, self, id, path_id);
    free(path_id);
    return 0;
}

static int slice_build(Effect *self, EngineCtx *ctx) {
    Slice *st = self->state;
    SliceConfig *cfg = &st->config;

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
    for (size_t i = 0; i < n; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        bool has_input_fg = ch->animation.has_input_fg;
        Color input_fg = ch->animation.input_fg_color;
        bool has_input_bg = ch->animation.has_input_bg;
        Color input_bg = ch->animation.input_bg_color;
        Coord input_coord = ch->input_coord;
        bool uses_pre = ch->uses_input_preexisting_colors;

        ColorPair final_colors;
        memset(&final_colors, 0, sizeof(final_colors));
        if (dynamic) {
            final_colors.has_fg = has_input_fg;
            final_colors.fg = input_fg;
            final_colors.has_bg = has_input_bg;
            final_colors.bg = input_bg;
        } else {
            const Color *cc = coordcolormap_get(&mapping, input_coord);
            final_colors.has_fg = true;
            if (cc) {
                final_colors.fg = *cc;
            }
        }
        animation_set_appearance(&ch->animation, uses_pre, ch->input_symbol, &final_colors);
    }
    free(characters);

    int64_t canvas_top = ctx->terminal.canvas.top;
    int64_t canvas_bottom = ctx->terminal.canvas.bottom;
    int64_t canvas_left = ctx->terminal.canvas.left;
    int64_t canvas_right = ctx->terminal.canvas.right;
    int64_t text_center_column = ctx->terminal.canvas.text_center_column;
    int64_t text_center_row = ctx->terminal.canvas.text_center_row;
    int64_t text_left = ctx->terminal.canvas.text_left;
    int64_t text_right = ctx->terminal.canvas.text_right;
    int64_t text_top = ctx->terminal.canvas.text_top;
    int64_t text_bottom = ctx->terminal.canvas.text_bottom;

    int rc = 0;
    if (cfg->slice_direction == SLICE_VERTICAL) {
        CharIdGrouping rows =
            terminal_get_characters_grouped(&ctx->terminal, character_filter_default(), CG_ROW_BOTTOM_TO_TOP);
        for (size_t r = 0; r < rows.len && rc == 0; r++) {
            CharIdBucket *row = &rows.buckets[r];
            for (size_t k = 0; k < row->len && rc == 0; k++) {
                CharId id = row->items[k];
                int64_t column = ctx->terminal.arena.items[id].input_coord.column;
                if (column <= text_center_column) {
                    if (slice_send_to(ctx, self, cfg, id, coord_new(column, canvas_top + 1)) != 0) {
                        rc = -1;
                    }
                }
            }
            CharIdBucket *opposite = &rows.buckets[rows.len - (r + 1)];
            for (size_t k = 0; k < opposite->len && rc == 0; k++) {
                CharId id = opposite->items[k];
                int64_t column = ctx->terminal.arena.items[id].input_coord.column;
                if (column > text_center_column) {
                    if (slice_send_to(ctx, self, cfg, id, coord_new(column, canvas_bottom - 1)) != 0) {
                        rc = -1;
                    }
                }
            }
            for (size_t k = 0; k < row->len; k++) {
                CharId id = row->items[k];
                if (ctx->terminal.arena.items[id].input_coord.column <= text_center_column) {
                    ac_insert(&ctx->active_characters, id);
                }
            }
            for (size_t k = 0; k < opposite->len; k++) {
                CharId id = opposite->items[k];
                if (ctx->terminal.arena.items[id].input_coord.column > text_center_column) {
                    ac_insert(&ctx->active_characters, id);
                }
            }
        }
        charidgrouping_free(&rows);
    } else if (cfg->slice_direction == SLICE_HORIZONTAL) {
        cfg->movement_speed *= 2.0;
        CharacterFilter filter;
        filter.input_chars = true;
        filter.inner_fill_chars = true;
        filter.outer_fill_chars = true;
        filter.added_chars = false;
        CharIdGrouping columns =
            terminal_get_characters_grouped(&ctx->terminal, filter, CG_COLUMN_RIGHT_TO_LEFT);
        CharIdGrouping trimmed;
        trimmed.buckets = NULL;
        trimmed.len = 0;
        size_t trim_cap = 0;
        for (size_t c = 0; c < columns.len; c++) {
            CharIdBucket *column = &columns.buckets[c];
            CharId *items = malloc((column->len ? column->len : 1) * sizeof(CharId));
            size_t count = 0;
            for (size_t k = 0; k < column->len; k++) {
                CharId id = column->items[k];
                Coord ic = ctx->terminal.arena.items[id].input_coord;
                if (text_left <= ic.column && ic.column <= text_right && text_bottom <= ic.row &&
                    ic.row <= text_top) {
                    items[count++] = id;
                }
            }
            if (count > 0) {
                if (trimmed.len == trim_cap) {
                    trim_cap = trim_cap ? trim_cap * 2 : 8;
                    trimmed.buckets = realloc(trimmed.buckets, trim_cap * sizeof(CharIdBucket));
                }
                trimmed.buckets[trimmed.len].items = items;
                trimmed.buckets[trimmed.len].len = count;
                trimmed.len++;
            } else {
                free(items);
            }
        }
        int64_t mid_point = text_center_row;
        for (size_t c = 0; c < trimmed.len && rc == 0; c++) {
            CharIdBucket *column = &trimmed.buckets[c];
            for (size_t k = 0; k < column->len && rc == 0; k++) {
                CharId id = column->items[k];
                int64_t row = ctx->terminal.arena.items[id].input_coord.row;
                if (row <= mid_point) {
                    if (slice_send_to(ctx, self, cfg, id, coord_new(canvas_left - 1, row)) != 0) {
                        rc = -1;
                    }
                }
            }
            CharIdBucket *opposite = &trimmed.buckets[trimmed.len - (c + 1)];
            for (size_t k = 0; k < opposite->len && rc == 0; k++) {
                CharId id = opposite->items[k];
                int64_t row = ctx->terminal.arena.items[id].input_coord.row;
                if (row > mid_point) {
                    if (slice_send_to(ctx, self, cfg, id, coord_new(canvas_right + 1, row)) != 0) {
                        rc = -1;
                    }
                }
            }
            for (size_t k = 0; k < column->len; k++) {
                CharId id = column->items[k];
                if (ctx->terminal.arena.items[id].input_coord.row <= mid_point) {
                    ac_insert(&ctx->active_characters, id);
                }
            }
            for (size_t k = 0; k < opposite->len; k++) {
                CharId id = opposite->items[k];
                if (ctx->terminal.arena.items[id].input_coord.row > mid_point) {
                    ac_insert(&ctx->active_characters, id);
                }
            }
        }
        charidgrouping_free(&columns);
        charidgrouping_free(&trimmed);
    } else if (cfg->slice_direction == SLICE_DIAGONAL) {
        CharIdGrouping diagonals = terminal_get_characters_grouped(
            &ctx->terminal, character_filter_default(), CG_DIAGONAL_BOTTOM_LEFT_TO_TOP_RIGHT);
        size_t half = diagonals.len / 2;
        size_t li = 0;
        size_t ri = half;
        while ((li < half || ri < diagonals.len) && rc == 0) {
            if (li < half) {
                CharIdBucket *group = &diagonals.buckets[li++];
                int64_t column = ctx->terminal.arena.items[group->items[0]].input_coord.column;
                Coord origin = coord_new(column, canvas_bottom - 1);
                for (size_t k = 0; k < group->len && rc == 0; k++) {
                    if (slice_send_to(ctx, self, cfg, group->items[k], origin) != 0) {
                        rc = -1;
                    }
                }
                for (size_t k = 0; k < group->len; k++) {
                    ac_insert(&ctx->active_characters, group->items[k]);
                }
            }
            if (ri < diagonals.len && rc == 0) {
                CharIdBucket *group = &diagonals.buckets[ri++];
                int64_t column = ctx->terminal.arena.items[group->items[group->len - 1]].input_coord.column;
                Coord origin = coord_new(column, canvas_top + 1);
                for (size_t k = 0; k < group->len && rc == 0; k++) {
                    if (slice_send_to(ctx, self, cfg, group->items[k], origin) != 0) {
                        rc = -1;
                    }
                }
                for (size_t k = 0; k < group->len; k++) {
                    ac_insert(&ctx->active_characters, group->items[k]);
                }
            }
        }
        charidgrouping_free(&diagonals);
    }

    CharId *active = NULL;
    size_t active_len = 0;
    ac_snapshot(&ctx->active_characters, &active, &active_len);
    for (size_t i = 0; i < active_len; i++) {
        terminal_set_character_visibility(&ctx->terminal, active[i], true);
    }
    free(active);

    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    return rc;
}

static const char *slice_next_frame(Effect *self, EngineCtx *ctx) {
    if (!ac_is_empty(&ctx->active_characters)) {
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void slice_destroy(Effect *self) {
    Slice *st = self->state;
    if (!st) {
        return;
    }
    free(st);
    free(self);
}

static const EffectOps SLICE_OPS = {slice_build, slice_next_frame, slice_destroy, NULL};

Effect *slice_make(const void *cfg) {
    Slice *st = calloc(1, sizeof(Slice));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const SliceConfig *)cfg;
    effect->ops = &SLICE_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec slice_specs[] = {
    {"slice-direction", 0, EF_CUSTOM, offsetof(SliceConfig, slice_direction), parse_slice_direction},
    EF_SPEC("movement-speed", 0, EF_FLOAT_POS, offsetof(SliceConfig, movement_speed)),
    EF_SPEC("movement-easing", 0, EF_EASING, offsetof(SliceConfig, movement_easing)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(SliceConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(SliceConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(SliceConfig, final_gradient_direction)),
};

const EffectEntry slice_entry = {
    "slice",
    slice_specs,
    sizeof(slice_specs) / sizeof(slice_specs[0]),
    sizeof(SliceConfig),
    slice_config_defaults,
    slice_free_config,
    slice_make,
};
