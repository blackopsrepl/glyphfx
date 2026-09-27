#include "effects/overflow.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/canvas.h"
#include "engine/character.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/geometry.h"
#include "utils/graphics.h"
#include "utils/pycompat.h"

typedef struct {
    CharId *characters;
    size_t len;
    size_t cap;
    bool final_;
} OverflowRow;

typedef struct {
    OverflowConfig config;
    // VecDeque<Row> pending_rows.
    OverflowRow *pending;
    size_t pending_len;
    size_t pending_cap;
    size_t pending_pos;
    // Vec<Row> active_rows.
    OverflowRow *active;
    size_t active_len;
    size_t active_cap;
    // HashMap<CharId, Color> (dense by arena slot).
    Color *character_final_color_map;
    bool *character_final_color_present;
    size_t character_final_color_len;
    int64_t delay;
    Gradient overflow_gradient;
    bool has_overflow_gradient;
} Overflow;

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

void overflow_config_defaults(void *cfg_ptr) {
    OverflowConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    push_color_default(&cfg->overflow_gradient_stops, "f2ebc0");
    push_color_default(&cfg->overflow_gradient_stops, "8dbfb3");
    push_color_default(&cfg->overflow_gradient_stops, "f2ebc0");
    cfg->overflow_cycles_range.start = 2;
    cfg->overflow_cycles_range.end = 4;
    cfg->overflow_speed = 3;
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "FFFFFF");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void overflow_free_config(void *cfg_ptr) {
    OverflowConfig *cfg = cfg_ptr;
    free(cfg->overflow_gradient_stops.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static void row_free(OverflowRow *row) {
    free(row->characters);
    row->characters = NULL;
    row->len = 0;
    row->cap = 0;
}

static void row_push(OverflowRow *row, CharId id) {
    if (row->len == row->cap) {
        size_t cap = row->cap ? row->cap * 2 : 8;
        CharId *grown = realloc(row->characters, cap * sizeof(CharId));
        if (!grown) {
            return;
        }
        row->characters = grown;
        row->cap = cap;
    }
    row->characters[row->len++] = id;
}

static void pending_push(Overflow *st, OverflowRow row) {
    if (st->pending_len == st->pending_cap) {
        size_t cap = st->pending_cap ? st->pending_cap * 2 : 16;
        OverflowRow *grown = realloc(st->pending, cap * sizeof(OverflowRow));
        if (!grown) {
            return;
        }
        st->pending = grown;
        st->pending_cap = cap;
    }
    st->pending[st->pending_len++] = row;
}

static bool pending_is_empty(const Overflow *st) {
    return st->pending_pos >= st->pending_len;
}

// Row.move_up.
static void row_move_up(EngineCtx *ctx, const OverflowRow *row) {
    for (size_t i = 0; i < row->len; i++) {
        Motion *motion = &ctx->terminal.arena.items[row->characters[i]].motion;
        Coord current = motion->current_coord;
        motion_set_coordinate(motion, coord_new(current.column, current.row + 1));
    }
}

// Row.setup.
static void row_setup(EngineCtx *ctx, const OverflowRow *row) {
    for (size_t i = 0; i < row->len; i++) {
        CharId id = row->characters[i];
        int64_t column = ctx->terminal.arena.items[id].input_coord.column;
        motion_set_coordinate(&ctx->terminal.arena.items[id].motion, coord_new(column, 0));
    }
}

// Row.set_color.
static void row_set_color(EngineCtx *ctx, const OverflowRow *row, const Color *fg_color) {
    for (size_t i = 0; i < row->len; i++) {
        CharId id = row->characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        bool uses_pre = ch->uses_input_preexisting_colors;
        ColorPair colors;
        memset(&colors, 0, sizeof(colors));
        colors.has_fg = true;
        colors.fg = *fg_color;
        animation_set_appearance(&ch->animation, uses_pre, ch->input_symbol, &colors);
    }
}

static void active_push(Overflow *st, OverflowRow row) {
    if (st->active_len == st->active_cap) {
        size_t cap = st->active_cap ? st->active_cap * 2 : 16;
        OverflowRow *grown = realloc(st->active, cap * sizeof(OverflowRow));
        if (!grown) {
            return;
        }
        st->active = grown;
        st->active_cap = cap;
    }
    st->active[st->active_len++] = row;
}

static int overflow_build(Effect *self, EngineCtx *ctx) {
    Overflow *st = self->state;
    OverflowConfig *cfg = &st->config;

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
                                                cfg->final_gradient_direction,
                                                &final_gradient_mapping) != 0) {
        gradient_free(&final_gradient);
        return -1;
    }

    CharacterFilter fills_filter;
    fills_filter.input_chars = true;
    fills_filter.inner_fill_chars = true;
    fills_filter.outer_fill_chars = true;
    fills_filter.added_chars = false;

    size_t characters_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, fills_filter,
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &characters_len);
    size_t arena_len = ctx->terminal.arena.len;
    st->character_final_color_len = arena_len;
    st->character_final_color_map = calloc(arena_len ? arena_len : 1, sizeof(Color));
    st->character_final_color_present = calloc(arena_len ? arena_len : 1, sizeof(bool));
    Color fallback;
    color_from_hex("000000", &fallback);
    for (size_t i = 0; i < characters_len; i++) {
        CharId id = characters[i];
        Coord coord = ctx->terminal.arena.items[id].input_coord;
        const Color *mapped = coordcolormap_get(&final_gradient_mapping, coord);
        if ((size_t)id < st->character_final_color_len) {
            st->character_final_color_present[id] = true;
            st->character_final_color_map[id] = mapped ? *mapped : fallback;
        }
    }

    int rc = 0;
    CharIdGrouping rows =
        terminal_get_characters_grouped(&ctx->terminal, character_filter_default(), CG_ROW_TOP_TO_BOTTOM);
    int64_t lower_range = cfg->overflow_cycles_range.start;
    int64_t upper_range = cfg->overflow_cycles_range.end;
    if (upper_range > 0) {
        int64_t cycles = rng_randint(&ctx->rng, lower_range, upper_range);
        for (int64_t c = 0; c < cycles && rc == 0; c++) {
            rng_shuffle(&ctx->rng, rows.buckets, rows.len, sizeof(CharIdBucket));
            for (size_t r = 0; r < rows.len && rc == 0; r++) {
                CharIdBucket *row = &rows.buckets[r];
                OverflowRow copied;
                memset(&copied, 0, sizeof(copied));
                copied.final_ = false;
                for (size_t k = 0; k < row->len && rc == 0; k++) {
                    CharId id = row->items[k];
                    EffectCharacter *ch = &ctx->terminal.arena.items[id];
                    char *symbol = malloc(strlen(ch->input_symbol) + 1);
                    strcpy(symbol, ch->input_symbol);
                    Coord coord = ch->input_coord;
                    char *fg_seq = ch->input_ansi_fg_sequence ? strdup(ch->input_ansi_fg_sequence) : NULL;
                    bool has_fg_seq = ch->has_input_fg_seq;
                    char *bg_seq = ch->input_ansi_bg_sequence ? strdup(ch->input_ansi_bg_sequence) : NULL;
                    bool has_bg_seq = ch->has_input_bg_seq;
                    bool no_color = ch->animation.no_color;
                    bool use_xterm = ch->animation.use_xterm_colors;
                    bool has_input_fg = ch->animation.has_input_fg;
                    Color input_fg = ch->animation.input_fg_color;
                    bool has_input_bg = ch->animation.has_input_bg;
                    Color input_bg = ch->animation.input_bg_color;

                    CharId copy_id = terminal_add_character(&ctx->terminal, symbol, coord);
                    free(symbol);
                    EffectCharacter *copy = &ctx->terminal.arena.items[copy_id];
                    copy->animation.existing_color_handling = ctx->terminal.config.existing_color_handling;
                    copy->uses_input_preexisting_colors = true;
                    free(copy->input_ansi_fg_sequence);
                    copy->input_ansi_fg_sequence = fg_seq;
                    copy->has_input_fg_seq = has_fg_seq;
                    free(copy->input_ansi_bg_sequence);
                    copy->input_ansi_bg_sequence = bg_seq;
                    copy->has_input_bg_seq = has_bg_seq;
                    copy->animation.no_color = no_color;
                    copy->animation.use_xterm_colors = use_xterm;
                    copy->animation.has_input_fg = has_input_fg;
                    copy->animation.input_fg_color = input_fg;
                    copy->animation.has_input_bg = has_input_bg;
                    copy->animation.input_bg_color = input_bg;
                    row_push(&copied, copy_id);
                }
                pending_push(st, copied);
            }
        }
    }

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    CharIdGrouping final_rows = terminal_get_characters_grouped(&ctx->terminal, fills_filter, CG_ROW_TOP_TO_BOTTOM);
    for (size_t r = 0; r < final_rows.len; r++) {
        CharIdBucket *row = &final_rows.buckets[r];
        for (size_t k = 0; k < row->len; k++) {
            CharId id = row->items[k];
            EffectCharacter *ch = &ctx->terminal.arena.items[id];
            // Pool symbols are interned and immutable, so the alias is safe.
            const char *current_symbol = visual_symbol(ch->animation.current_visual);
            char *current_symbol_copy = malloc(strlen(current_symbol) + 1);
            strcpy(current_symbol_copy, current_symbol);
            bool uses_pre = ch->uses_input_preexisting_colors;
            ColorPair colors;
            memset(&colors, 0, sizeof(colors));
            if (dynamic) {
                if (ch->animation.has_input_fg || ch->animation.has_input_bg) {
                    colors.has_fg = ch->animation.has_input_fg;
                    colors.fg = ch->animation.input_fg_color;
                    colors.has_bg = ch->animation.has_input_bg;
                    colors.bg = ch->animation.input_bg_color;
                }
                animation_set_appearance(&ch->animation, uses_pre, current_symbol_copy, &colors);
            } else {
                Color final_color = fallback;
                if ((size_t)id < st->character_final_color_len &&
                    st->character_final_color_present[id]) {
                    final_color = st->character_final_color_map[id];
                }
                colors.has_fg = true;
                colors.fg = final_color;
                animation_set_appearance(&ch->animation, uses_pre, current_symbol_copy, &colors);
            }
            free(current_symbol_copy);
        }
        OverflowRow final_row;
        memset(&final_row, 0, sizeof(final_row));
        final_row.final_ = true;
        for (size_t k = 0; k < row->len; k++) {
            row_push(&final_row, row->items[k]);
        }
        pending_push(st, final_row);
    }
    charidgrouping_free(&rows);
    charidgrouping_free(&final_rows);

    st->delay = 0;
    int64_t divisor = (int64_t)cfg->overflow_gradient_stops.len - 1;
    if (divisor < 1) {
        divisor = 1;
    }
    int64_t steps = py_floor_div(ctx->terminal.canvas.top, divisor);
    if (steps < 1) {
        steps = 1;
    }
    if (gradient_with_steps(cfg->overflow_gradient_stops.items, cfg->overflow_gradient_stops.len, steps, false,
                            &st->overflow_gradient) != 0) {
        rc = -1;
    } else {
        st->has_overflow_gradient = true;
    }

    free(characters);
    coordcolormap_free(&final_gradient_mapping);
    gradient_free(&final_gradient);
    return rc;
}

static const char *overflow_next_frame(Effect *self, EngineCtx *ctx) {
    Overflow *st = self->state;
    if (!pending_is_empty(st)) {
        if (st->delay == 0) {
            int64_t reps = rng_randint(&ctx->rng, 1, st->config.overflow_speed);
            for (int64_t r = 0; r < reps; r++) {
                if (!pending_is_empty(st)) {
                    for (size_t i = 0; i < st->active_len; i++) {
                        OverflowRow *row = &st->active[i];
                        row_move_up(ctx, row);
                        if (!row->final_) {
                            CharId head = row->characters[0];
                            int64_t head_row =
                                ctx->terminal.arena.items[head].motion.current_coord.row;
                            int64_t last = (int64_t)st->overflow_gradient.len - 1;
                            int64_t index = head_row < last ? head_row : last;
                            if (index < 0) {
                                index = 0;
                            }
                            row_set_color(ctx, row, &st->overflow_gradient.spectrum[index]);
                        }
                    }
                    OverflowRow next_row = st->pending[st->pending_pos];
                    memset(&st->pending[st->pending_pos], 0, sizeof(OverflowRow));
                    st->pending_pos++;
                    row_setup(ctx, &next_row);
                    row_move_up(ctx, &next_row);
                    if (!next_row.final_) {
                        row_set_color(ctx, &next_row, &st->overflow_gradient.spectrum[0]);
                    }
                    for (size_t i = 0; i < next_row.len; i++) {
                        terminal_set_character_visibility(&ctx->terminal, next_row.characters[i], true);
                    }
                    active_push(st, next_row);
                }
            }
            st->delay = rng_randint(&ctx->rng, 0, 3);
        } else {
            st->delay -= 1;
        }

        int64_t canvas_top = ctx->terminal.canvas.top;
        size_t out = 0;
        for (size_t i = 0; i < st->active_len; i++) {
            OverflowRow row = st->active[i];
            CharId head = row.characters[0];
            if (ctx->terminal.arena.items[head].motion.current_coord.row <= canvas_top) {
                st->active[out++] = row;
            } else {
                row_free(&row);
            }
        }
        st->active_len = out;

        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void overflow_destroy(Effect *self) {
    Overflow *st = self->state;
    if (!st) {
        return;
    }
    for (size_t i = 0; i < st->pending_len; i++) {
        row_free(&st->pending[i]);
    }
    for (size_t i = 0; i < st->active_len; i++) {
        row_free(&st->active[i]);
    }
    free(st->pending);
    free(st->active);
    free(st->character_final_color_map);
    free(st->character_final_color_present);
    if (st->has_overflow_gradient) {
        gradient_free(&st->overflow_gradient);
    }
    free(st);
    free(self);
}

static const EffectOps OVERFLOW_OPS = {overflow_build, overflow_next_frame, overflow_destroy, NULL};

Effect *overflow_make(const void *cfg) {
    Overflow *st = calloc(1, sizeof(Overflow));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const OverflowConfig *)cfg;
    effect->ops = &OVERFLOW_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec overflow_specs[] = {
    EF_SPEC("overflow-gradient-stops", 0, EF_COLOR_LIST, offsetof(OverflowConfig, overflow_gradient_stops)),
    EF_SPEC("overflow-cycles-range", 0, EF_INT_RANGE, offsetof(OverflowConfig, overflow_cycles_range)),
    EF_SPEC("overflow-speed", 0, EF_POS_INT, offsetof(OverflowConfig, overflow_speed)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(OverflowConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(OverflowConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(OverflowConfig, final_gradient_direction)),
};

const EffectEntry overflow_entry = {
    "overflow",
    overflow_specs,
    sizeof(overflow_specs) / sizeof(overflow_specs[0]),
    sizeof(OverflowConfig),
    overflow_config_defaults,
    overflow_free_config,
    overflow_make,
};
