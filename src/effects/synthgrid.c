// synthgrid, ported from the reference effects/synthgrid.rs.
//
// Grid lines live in a Vec; groups are (group_number, Vec<CharId>) tuples. The
// SCENE_COMPLETE-driven group tracker is a Vec indexed by group_number,
// decremented via the CB_UPDATE_GROUP_TRACKER effect callback. Row/column
// symbols are single symbols; text-generation-symbols is a symbol list.
#include "effects/synthgrid.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/motion.h"
#include "engine/terminal.h"
#include "utils/easing.h"
#include "utils/graphics.h"
#include "utils/ordmap.h"
#include "utils/pycompat.h"

/// Callback id: update_group_tracker(group_number).
#define CB_UPDATE_GROUP_TRACKER 0u

typedef enum {
    SG_HORIZONTAL,
    SG_VERTICAL,
} SgDirection;

typedef enum {
    SG_GRID_EXPAND,
    SG_ADD_CHARS,
    SG_COLLAPSE,
    SG_COMPLETE,
} SgPhase;

typedef struct {
    SgDirection direction;
    CharId *collapsed;
    size_t collapsed_len;
    size_t collapsed_cap;
    CharId *extended;
    size_t extended_len;
    size_t extended_cap;
} GridLine;

typedef struct {
    int64_t group_number;
    CharId *items;
    size_t len;
} PendingGroup;

typedef struct {
    Effect *effect;
    SynthGridConfig config;
    PendingGroup *pending;
    size_t pending_len;
    size_t pending_cap;
    GridLine *lines;
    size_t lines_len;
    size_t lines_cap;
    int64_t *group_tracker;
    size_t tracker_len;
    ColorPair *final_map;
    bool *final_present;
    size_t map_len;
    SgPhase phase;
    size_t total_group_count;
    int64_t active_groups;
} SynthGrid;

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

static void push_symbol_default(StringList *list, const char *symbol) {
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

static void idvec_push(CharId **items, size_t *len, size_t *cap, CharId id) {
    if (*len == *cap) {
        size_t next = *cap ? *cap * 2 : 8;
        CharId *grown = realloc(*items, next * sizeof(CharId));
        if (!grown) {
            return;
        }
        *items = grown;
        *cap = next;
    }
    (*items)[(*len)++] = id;
}

static void i64vec_push(int64_t **items, size_t *len, size_t *cap, int64_t v) {
    if (*len == *cap) {
        size_t next = *cap ? *cap * 2 : 8;
        int64_t *grown = realloc(*items, next * sizeof(int64_t));
        if (!grown) {
            return;
        }
        *items = grown;
        *cap = next;
    }
    (*items)[(*len)++] = v;
}

static void line_push(SynthGrid *st, GridLine line) {
    if (st->lines_len == st->lines_cap) {
        size_t next = st->lines_cap ? st->lines_cap * 2 : 8;
        GridLine *grown = realloc(st->lines, next * sizeof(GridLine));
        if (!grown) {
            return;
        }
        st->lines = grown;
        st->lines_cap = next;
    }
    st->lines[st->lines_len++] = line;
}

static void pending_push(SynthGrid *st, PendingGroup group) {
    if (st->pending_len == st->pending_cap) {
        size_t next = st->pending_cap ? st->pending_cap * 2 : 8;
        PendingGroup *grown = realloc(st->pending, next * sizeof(PendingGroup));
        if (!grown) {
            return;
        }
        st->pending = grown;
        st->pending_cap = next;
    }
    st->pending[st->pending_len++] = group;
}

static void gridline_free(GridLine *line) {
    free(line->collapsed);
    free(line->extended);
    line->collapsed = NULL;
    line->collapsed_len = 0;
    line->collapsed_cap = 0;
    line->extended = NULL;
    line->extended_len = 0;
    line->extended_cap = 0;
}

void synthgrid_config_defaults(void *cfg_ptr) {
    SynthGridConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    push_color_default(&cfg->grid_gradient_stops, "CC00CC");
    push_color_default(&cfg->grid_gradient_stops, "ffffff");
    push_int_default(&cfg->grid_gradient_steps, 12);
    cfg->grid_gradient_direction = GRADIENT_DIAGONAL;
    push_color_default(&cfg->text_gradient_stops, "8A008A");
    push_color_default(&cfg->text_gradient_stops, "00D1FF");
    push_color_default(&cfg->text_gradient_stops, "FFFFFF");
    push_int_default(&cfg->text_gradient_steps, 12);
    cfg->text_gradient_direction = GRADIENT_VERTICAL;
    cfg->grid_row_symbol = malloc(4);
    strcpy(cfg->grid_row_symbol, "\xe2\x94\x80");
    cfg->grid_column_symbol = malloc(4);
    strcpy(cfg->grid_column_symbol, "\xe2\x94\x82");
    push_symbol_default(&cfg->text_generation_symbols, "\xe2\x96\x91");
    push_symbol_default(&cfg->text_generation_symbols, "\xe2\x96\x92");
    push_symbol_default(&cfg->text_generation_symbols, "\xe2\x96\x93");
    cfg->max_active_blocks = 0.1;
}

void synthgrid_free_config(void *cfg_ptr) {
    SynthGridConfig *cfg = cfg_ptr;
    free(cfg->grid_gradient_stops.items);
    free(cfg->grid_gradient_steps.items);
    free(cfg->text_gradient_stops.items);
    free(cfg->text_gradient_steps.items);
    free(cfg->grid_row_symbol);
    free(cfg->grid_column_symbol);
    for (size_t i = 0; i < cfg->text_generation_symbols.len; i++) {
        free(cfg->text_generation_symbols.items[i]);
    }
    free(cfg->text_generation_symbols.items);
}

/// SynthGridIterator.find_even_gap.
static int64_t sg_find_even_gap(int64_t dimension) {
    dimension -= 2;
    if (dimension <= 0) {
        return 0;
    }
    // [i for i in range(dimension, 4, -1) if dimension % i <= 1]
    int64_t *potential_gaps = NULL;
    size_t gaps_len = 0;
    size_t gaps_cap = 0;
    for (int64_t i = dimension; i > 4; i--) {
        if (dimension % i <= 1) {
            i64vec_push(&potential_gaps, &gaps_len, &gaps_cap, i);
        }
    }
    if (gaps_len == 0) {
        free(potential_gaps);
        return 4;
    }
    // min(potential_gaps, key=lambda x: abs(x - dimension // 5)) — first minimum wins
    int64_t target = py_floor_div(dimension, 5);
    int64_t best = potential_gaps[0];
    int64_t best_key = llabs(potential_gaps[0] - target);
    for (size_t i = 1; i < gaps_len; i++) {
        int64_t key = llabs(potential_gaps[i] - target);
        if (key < best_key) {
            best = potential_gaps[i];
            best_key = key;
        }
    }
    free(potential_gaps);
    return best;
}

/// GridLine.__init__ (needs EffectHooks for activate_scene, so lives here).
static int sg_make_grid_line(SynthGrid *st, EngineCtx *ctx, Effect *effect, Coord origin, SgDirection direction,
                             const CoordColorMap *grid_gradient_mapping, GridLine *out) {
    const char *grid_symbol =
        direction == SG_HORIZONTAL ? st->config.grid_row_symbol : st->config.grid_column_symbol;
    Easing ease;
    memset(&ease, 0, sizeof(ease));
    out->direction = direction;
    out->collapsed = NULL;
    out->collapsed_len = 0;
    out->collapsed_cap = 0;
    out->extended = NULL;
    out->extended_len = 0;
    out->extended_cap = 0;

    int64_t first;
    int64_t last;
    bool horizontal = direction == SG_HORIZONTAL;
    if (horizontal) {
        first = ctx->terminal.canvas.left;
        last = ctx->terminal.canvas.right;
    } else {
        first = ctx->terminal.canvas.bottom;
        last = ctx->terminal.canvas.top;
    }
    for (int64_t index = first; index <= last; index++) {
        if (!horizontal && index >= last) {
            break;
        }
        Coord coord = horizontal ? coord_new(index, origin.row) : coord_new(origin.column, index);
        CharId id = terminal_add_character(&ctx->terminal, grid_symbol, coord_new(0, 0));
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        bool uses_pre = ch->uses_input_preexisting_colors;
        const char *grid_scn = animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, ease, "",
                                                    uses_pre);
        const Color *fg = coordcolormap_get(grid_gradient_mapping, coord);
        Scene *scene =
            grid_scn ? (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, grid_scn) : NULL;
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors.has_fg = true;
        if (fg) {
            vp.colors.fg = *fg;
        }
        if (!scene || scene_add_frame(scene, grid_symbol, 1, &vp) != 0) {
            gridline_free(out);
            return -1;
        }
        engine_activate_scene(ctx, effect, id, grid_scn);
        ctx->terminal.arena.items[id].layer = 2;
        motion_set_coordinate(&ctx->terminal.arena.items[id].motion, coord);
        idvec_push(&out->collapsed, &out->collapsed_len, &out->collapsed_cap, id);
    }
    return 0;
}

/// GridLine.extend.
static void gridline_extend(GridLine *line, EngineCtx *ctx) {
    size_t count = line->direction == SG_HORIZONTAL ? 3 : 1;
    for (size_t i = 0; i < count; i++) {
        if (line->collapsed_len > 0) {
            CharId next = line->collapsed[0];
            memmove(line->collapsed, line->collapsed + 1, (line->collapsed_len - 1) * sizeof(CharId));
            line->collapsed_len--;
            terminal_set_character_visibility(&ctx->terminal, next, true);
            idvec_push(&line->extended, &line->extended_len, &line->extended_cap, next);
        }
    }
}

/// GridLine.collapse.
static void gridline_collapse(GridLine *line, EngineCtx *ctx) {
    size_t count = line->direction == SG_HORIZONTAL ? 3 : 1;
    if (line->collapsed_len == 0) {
        for (size_t i = 0; i < line->extended_len / 2; i++) {
            CharId tmp = line->extended[i];
            line->extended[i] = line->extended[line->extended_len - 1 - i];
            line->extended[line->extended_len - 1 - i] = tmp;
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (line->extended_len > 0) {
            CharId next = line->extended[0];
            memmove(line->extended, line->extended + 1, (line->extended_len - 1) * sizeof(CharId));
            line->extended_len--;
            terminal_set_character_visibility(&ctx->terminal, next, false);
            idvec_push(&line->collapsed, &line->collapsed_len, &line->collapsed_cap, next);
        }
    }
}

static bool gridline_is_extended(const GridLine *line) {
    return line->collapsed_len == 0;
}

static bool gridline_is_collapsed(const GridLine *line) {
    return line->extended_len == 0;
}

static void synthgrid_callback(Effect *self, EngineCtx *ctx, CharId character, const EffectCallback *cb) {
    (void)ctx;
    (void)character;
    SynthGrid *st = self->state;
    if (cb->id == CB_UPDATE_GROUP_TRACKER) {
        if (cb->args_len > 0 && cb->args[0].kind == CALLBACK_INT) {
            int64_t group_number = cb->args[0].i;
            if (group_number >= 0 && (size_t)group_number < st->tracker_len) {
                st->group_tracker[group_number] -= 1;
            }
        }
    }
}

static int synthgrid_build(Effect *self, EngineCtx *ctx) {
    SynthGrid *st = self->state;
    SynthGridConfig *cfg = &st->config;
    int rc = 0;

    Gradient grid_gradient;
    memset(&grid_gradient, 0, sizeof(grid_gradient));
    if (gradient_new(cfg->grid_gradient_stops.items, cfg->grid_gradient_stops.len, cfg->grid_gradient_steps.items,
                     cfg->grid_gradient_steps.len, false, false, &grid_gradient) != 0) {
        return -1;
    }
    CoordColorMap grid_mapping;
    memset(&grid_mapping, 0, sizeof(grid_mapping));
    if (gradient_build_coordinate_color_mapping(&grid_gradient, 1, ctx->terminal.canvas.top, 1,
                                                ctx->terminal.canvas.right, cfg->grid_gradient_direction,
                                                &grid_mapping) != 0) {
        gradient_free(&grid_gradient);
        return -1;
    }
    Gradient text_gradient;
    memset(&text_gradient, 0, sizeof(text_gradient));
    if (gradient_new(cfg->text_gradient_stops.items, cfg->text_gradient_stops.len, cfg->text_gradient_steps.items,
                     cfg->text_gradient_steps.len, false, false, &text_gradient) != 0) {
        coordcolormap_free(&grid_mapping);
        gradient_free(&grid_gradient);
        return -1;
    }
    CoordColorMap text_mapping;
    memset(&text_mapping, 0, sizeof(text_mapping));
    if (gradient_build_coordinate_color_mapping(&text_gradient, ctx->terminal.canvas.text_bottom,
                                                ctx->terminal.canvas.text_top, ctx->terminal.canvas.text_left,
                                                ctx->terminal.canvas.text_right, cfg->text_gradient_direction,
                                                &text_mapping) != 0) {
        gradient_free(&text_gradient);
        coordcolormap_free(&grid_mapping);
        gradient_free(&grid_gradient);
        return -1;
    }

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    size_t chars_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &chars_len);
    st->map_len = ctx->terminal.arena.len;
    st->final_map = calloc(st->map_len ? st->map_len : 1, sizeof(ColorPair));
    st->final_present = calloc(st->map_len ? st->map_len : 1, sizeof(bool));
    for (size_t i = 0; i < chars_len; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        ColorPair colors;
        memset(&colors, 0, sizeof(colors));
        if (dynamic) {
            colors.has_fg = ch->animation.has_input_fg;
            colors.fg = ch->animation.input_fg_color;
            colors.has_bg = ch->animation.has_input_bg;
            colors.bg = ch->animation.input_bg_color;
        } else if (strcmp(ch->input_symbol, " ") != 0) {
            const Color *mapped = coordcolormap_get(&text_mapping, ch->input_coord);
            colors.has_fg = true;
            if (mapped) {
                colors.fg = *mapped;
            }
        }
        if ((size_t)id < st->map_len) {
            st->final_map[id] = colors;
            st->final_present[id] = true;
        }
    }
    free(characters);

    int64_t c_left = ctx->terminal.canvas.left;
    int64_t c_right = ctx->terminal.canvas.right;
    int64_t c_bottom = ctx->terminal.canvas.bottom;
    int64_t c_top = ctx->terminal.canvas.top;

    GridLine line;
    if (sg_make_grid_line(st, ctx, self, coord_new(c_left, c_bottom), SG_HORIZONTAL, &grid_mapping, &line) != 0) {
        rc = -1;
    } else {
        line_push(st, line);
        if (sg_make_grid_line(st, ctx, self, coord_new(c_left, c_top), SG_HORIZONTAL, &grid_mapping, &line) != 0) {
            rc = -1;
        } else {
            line_push(st, line);
            if (sg_make_grid_line(st, ctx, self, coord_new(c_left, c_bottom), SG_VERTICAL, &grid_mapping,
                                  &line) != 0) {
                rc = -1;
            } else {
                line_push(st, line);
                if (sg_make_grid_line(st, ctx, self, coord_new(c_right, c_bottom), SG_VERTICAL, &grid_mapping,
                                      &line) != 0) {
                    rc = -1;
                } else {
                    line_push(st, line);
                }
            }
        }
    }
    int64_t row_gap;
    int64_t column_gap;
    if (rc == 0) {
        if (c_top > 2 * c_right) {
            row_gap = sg_find_even_gap(c_top) + 1;
            column_gap = row_gap * 2;
        } else {
            column_gap = sg_find_even_gap(c_right) + 1;
            row_gap = py_floor_div(column_gap, 2);
        }
    } else {
        row_gap = 1;
        column_gap = 1;
    }

    int64_t *row_indexes = NULL;
    size_t row_indexes_len = 0;
    size_t row_indexes_cap = 0;
    int64_t *column_indexes = NULL;
    size_t column_indexes_len = 0;
    size_t column_indexes_cap = 0;

    if (rc == 0) {
        // range(bottom + row_gap, top, max(row_gap, 1))
        int64_t row_step = row_gap > 1 ? row_gap : 1;
        int64_t row_index = c_bottom + row_gap;
        while (row_index < c_top) {
            if (c_top - row_index >= 2) {
                i64vec_push(&row_indexes, &row_indexes_len, &row_indexes_cap, row_index);
                if (sg_make_grid_line(st, ctx, self, coord_new(c_left, row_index), SG_HORIZONTAL, &grid_mapping,
                                      &line) != 0) {
                    rc = -1;
                    break;
                }
                line_push(st, line);
            }
            row_index += row_step;
        }
    }
    if (rc == 0) {
        // range(left + column_gap, right, max(column_gap, 1))
        int64_t column_step = column_gap > 1 ? column_gap : 1;
        int64_t column_index = c_left + column_gap;
        while (column_index < c_right) {
            if (c_right - column_index >= 2) {
                i64vec_push(&column_indexes, &column_indexes_len, &column_indexes_cap, column_index);
                if (sg_make_grid_line(st, ctx, self, coord_new(column_index, c_bottom), SG_VERTICAL,
                                      &grid_mapping, &line) != 0) {
                    rc = -1;
                    break;
                }
                line_push(st, line);
            }
            column_index += column_step;
        }
    }

    if (rc == 0) {
        i64vec_push(&row_indexes, &row_indexes_len, &row_indexes_cap, c_top + 1);
        i64vec_push(&column_indexes, &column_indexes_len, &column_indexes_cap, c_right + 1);

        int64_t prev_row_index = 1;
        for (size_t ri = 0; ri < row_indexes_len && rc == 0; ri++) {
            int64_t row_index = row_indexes[ri];
            int64_t prev_column_index = 1;
            for (size_t ci = 0; ci < column_indexes_len && rc == 0; ci++) {
                int64_t column_index = column_indexes[ci];
                CoordVec coords_in_block;
                coordvec_init(&coords_in_block);
                if (row_index == c_top) {
                    // make sure the top row is included
                    row_index += 1;
                }
                for (int64_t row = prev_row_index; row < row_index; row++) {
                    for (int64_t column = prev_column_index; column < column_index; column++) {
                        coordvec_push(&coords_in_block, coord_new(column, row));
                    }
                }
                CharId *characters_in_block = NULL;
                size_t block_len = 0;
                size_t block_cap = 0;
                for (size_t k = 0; k < coords_in_block.len; k++) {
                    CharId id = terminal_get_character_by_input_coord(&ctx->terminal, coords_in_block.items[k]);
                    if (id != CHAR_ID_NONE) {
                        idvec_push(&characters_in_block, &block_len, &block_cap, id);
                    }
                }
                coordvec_free(&coords_in_block);
                if (block_len > 0) {
                    PendingGroup group;
                    group.group_number = (int64_t)st->pending_len;
                    group.items = characters_in_block;
                    group.len = block_len;
                    pending_push(st, group);
                } else {
                    free(characters_in_block);
                }
                prev_column_index = column_index;
            }
            prev_row_index = row_index;
        }
    }

    if (rc == 0) {
        st->tracker_len = st->pending_len;
        st->group_tracker = calloc(st->tracker_len ? st->tracker_len : 1, sizeof(int64_t));
        Easing ease;
        memset(&ease, 0, sizeof(ease));
        for (size_t g = 0; g < st->pending_len && rc == 0; g++) {
            PendingGroup *group = &st->pending[g];
            for (size_t k = 0; k < group->len && rc == 0; k++) {
                CharId character = group->items[k];
                EffectCharacter *ch = &ctx->terminal.arena.items[character];
                char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
                strcpy(input_symbol, ch->input_symbol);
                bool uses_pre = ch->uses_input_preexisting_colors;
                const char *dissolve_scn = animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false,
                                                               ease, "", uses_pre);
                Scene *scene = dissolve_scn
                                   ? (Scene *)om_get(&ctx->terminal.arena.items[character].animation.scenes,
                                                     dissolve_scn)
                                   : NULL;
                int64_t frame_count = rng_randint(&ctx->rng, 15, 30);
                for (int64_t f = 0; f < frame_count && rc == 0; f++) {
                    const char *symbol = cfg->text_generation_symbols
                                             .items[rng_choice_index(&ctx->rng, cfg->text_generation_symbols.len)];
                    Color fg = text_gradient.spectrum[rng_choice_index(&ctx->rng, text_gradient.len)];
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    vp.colors.has_fg = true;
                    vp.colors.fg = fg;
                    if (!scene || scene_add_frame(scene, symbol, 2, &vp) != 0) {
                        rc = -1;
                    }
                }
                ColorPair final_colors;
                memset(&final_colors, 0, sizeof(final_colors));
                if ((size_t)character < st->map_len && st->final_present[character]) {
                    final_colors = st->final_map[character];
                }
                if (rc == 0) {
                    VisualParams fvp;
                    memset(&fvp, 0, sizeof(fvp));
                    fvp.has_colors = true;
                    fvp.colors = final_colors;
                    if (!scene || scene_add_frame(scene, input_symbol, 1, &fvp) != 0) {
                        rc = -1;
                    }
                }
                if (rc == 0) {
                    engine_activate_scene(ctx, self, character, dissolve_scn);
                    CallerKey caller;
                    memset(&caller, 0, sizeof(caller));
                    caller.kind = CALLER_SCENE;
                    caller.id = (char *)dissolve_scn;
                    EventAction action;
                    memset(&action, 0, sizeof(action));
                    action.kind = ACTION_CALLBACK;
                    action.cb.id = CB_UPDATE_GROUP_TRACKER;
                    action.cb.args = malloc(sizeof(CallbackValue));
                    action.cb.args_len = 1;
                    action.cb.args[0].kind = CALLBACK_INT;
                    action.cb.args[0].i = group->group_number;
                    if (engine_register_event(ctx, character, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
                        rc = -1;
                    }
                    free(action.cb.args);
                }
                free(input_symbol);
            }
        }
    }

    if (rc == 0) {
        rng_shuffle(&ctx->rng, st->pending, st->pending_len, sizeof(PendingGroup));
        st->phase = SG_GRID_EXPAND;
        st->total_group_count = st->pending_len;
        if (st->total_group_count == 0) {
            size_t n = 0;
            CharId *all = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                  CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);
            for (size_t i = 0; i < n; i++) {
                terminal_set_character_visibility(&ctx->terminal, all[i], true);
                ac_insert(&ctx->active_characters, all[i]);
            }
            free(all);
        }
        st->active_groups = 0;
    }

    free(row_indexes);
    free(column_indexes);
    coordcolormap_free(&text_mapping);
    coordcolormap_free(&grid_mapping);
    gradient_free(&text_gradient);
    gradient_free(&grid_gradient);
    return rc;
}

static const char *synthgrid_next_frame(Effect *self, EngineCtx *ctx) {
    SynthGrid *st = self->state;
    if (!(st->pending_len > 0 || !ac_is_empty(&ctx->active_characters) || st->phase != SG_COMPLETE)) {
        return NULL;
    }
    switch (st->phase) {
        case SG_GRID_EXPAND: {
            bool all_extended = true;
            for (size_t i = 0; i < st->lines_len; i++) {
                if (!gridline_is_extended(&st->lines[i])) {
                    all_extended = false;
                    break;
                }
            }
            if (!all_extended) {
                for (size_t i = 0; i < st->lines_len; i++) {
                    if (!gridline_is_extended(&st->lines[i])) {
                        gridline_extend(&st->lines[i], ctx);
                    }
                }
            } else {
                st->phase = SG_ADD_CHARS;
            }
            break;
        }
        case SG_ADD_CHARS: {
            if (st->pending_len > 0 &&
                (double)st->active_groups < (double)st->total_group_count * st->config.max_active_blocks) {
                PendingGroup group = st->pending[0];
                memmove(st->pending, st->pending + 1, (st->pending_len - 1) * sizeof(PendingGroup));
                st->pending_len--;
                for (size_t k = 0; k < group.len; k++) {
                    CharId ch = group.items[k];
                    terminal_set_character_visibility(&ctx->terminal, ch, true);
                    ac_insert(&ctx->active_characters, ch);
                    if (group.group_number >= 0 && (size_t)group.group_number < st->tracker_len) {
                        st->group_tracker[group.group_number] += 1;
                    }
                }
                free(group.items);
            }
            if (st->pending_len == 0 && ac_is_empty(&ctx->active_characters) && st->active_groups == 0) {
                st->phase = SG_COLLAPSE;
            }
            break;
        }
        case SG_COLLAPSE: {
            bool all_collapsed = true;
            for (size_t i = 0; i < st->lines_len; i++) {
                if (!gridline_is_collapsed(&st->lines[i])) {
                    all_collapsed = false;
                    break;
                }
            }
            if (!all_collapsed) {
                for (size_t i = 0; i < st->lines_len; i++) {
                    if (!gridline_is_collapsed(&st->lines[i])) {
                        gridline_collapse(&st->lines[i], ctx);
                    }
                }
            } else {
                st->phase = SG_COMPLETE;
            }
            break;
        }
        case SG_COMPLETE:
            break;
    }
    engine_update(ctx, self);
    st->active_groups = 0;
    for (size_t i = 0; i < st->tracker_len; i++) {
        if (st->group_tracker[i] != 0) {
            st->active_groups += 1;
        }
    }
    return engine_frame(ctx);
}

static void synthgrid_destroy(Effect *self) {
    SynthGrid *st = self->state;
    if (!st) {
        return;
    }
    for (size_t i = 0; i < st->pending_len; i++) {
        free(st->pending[i].items);
    }
    free(st->pending);
    for (size_t i = 0; i < st->lines_len; i++) {
        gridline_free(&st->lines[i]);
    }
    free(st->lines);
    free(st->group_tracker);
    free(st->final_map);
    free(st->final_present);
    free(st);
    free(self);
}

static const EffectOps SYNTHGRID_OPS = {synthgrid_build, synthgrid_next_frame, synthgrid_destroy,
                                        synthgrid_callback};

Effect *synthgrid_make(const void *cfg) {
    SynthGrid *st = calloc(1, sizeof(SynthGrid));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const SynthGridConfig *)cfg;
    st->effect = effect;
    effect->ops = &SYNTHGRID_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec synthgrid_specs[] = {
    EF_SPEC("grid-gradient-stops", 0, EF_COLOR_LIST, offsetof(SynthGridConfig, grid_gradient_stops)),
    EF_SPEC("grid-gradient-steps", 0, EF_INT_LIST, offsetof(SynthGridConfig, grid_gradient_steps)),
    EF_SPEC("grid-gradient-direction", 0, EF_DIRECTION, offsetof(SynthGridConfig, grid_gradient_direction)),
    EF_SPEC("text-gradient-stops", 0, EF_COLOR_LIST, offsetof(SynthGridConfig, text_gradient_stops)),
    EF_SPEC("text-gradient-steps", 0, EF_INT_LIST, offsetof(SynthGridConfig, text_gradient_steps)),
    EF_SPEC("text-gradient-direction", 0, EF_DIRECTION, offsetof(SynthGridConfig, text_gradient_direction)),
    EF_SPEC("grid-row-symbol", 0, EF_SYMBOL, offsetof(SynthGridConfig, grid_row_symbol)),
    EF_SPEC("grid-column-symbol", 0, EF_SYMBOL, offsetof(SynthGridConfig, grid_column_symbol)),
    {"text-generation-symbols", 0, EF_STRING_LIST, offsetof(SynthGridConfig, text_generation_symbols), NULL},
    EF_SPEC("max-active-blocks", 0, EF_RATIO_POS, offsetof(SynthGridConfig, max_active_blocks)),
};

const EffectEntry synthgrid_entry = {
    "synthgrid",
    synthgrid_specs,
    sizeof(synthgrid_specs) / sizeof(synthgrid_specs[0]),
    sizeof(SynthGridConfig),
    synthgrid_config_defaults,
    synthgrid_free_config,
    synthgrid_make,
};
