// matrix, ported from the reference effects/matrix.rs.
//
// THE CLOCK EFFECT: rain_start is taken via clock.now_wall() after build and the
// rain phase ends at effect_matrix.py:549 when now_wall() - rain_start exceeds
// rain_time. The parity harness virtualizes the clock (1/frame_rate per emitted
// frame), so the deadline is deterministic. The inner RainColumn is the
// RainColumn struct; columns are compared by identity upstream, so the
// pending/active/full lists hold indices into a Vec arena.
#include "effects/matrix.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/easing.h"
#include "utils/graphics.h"
#include "utils/ordmap.h"
#include "utils/pycompat.h"

typedef enum {
    COLUMN_RAIN,
    COLUMN_FILL,
} ColumnPhase;

typedef enum {
    PHASE_RAIN,
    PHASE_FILL,
    PHASE_RESOLVE,
} Phase;

typedef struct {
    CharId *characters;
    size_t characters_len;
    CharId *pending;
    size_t pending_len;
    size_t pending_cap;
    CharId *visible;
    size_t visible_len;
    size_t visible_cap;
    ColumnPhase phase;
    double column_drop_chance;
    int64_t base_rain_fall_delay;
    int64_t active_rain_fall_delay;
    size_t length;
    int64_t hold_time;
} RainColumn;

typedef struct {
    MatrixConfig config;
    RainColumn *columns;
    size_t columns_len;
    size_t columns_cap;
    size_t *pending_columns;
    size_t pending_columns_len;
    size_t pending_columns_cap;
    size_t *active_columns;
    size_t active_columns_len;
    size_t active_columns_cap;
    size_t *full_columns;
    size_t full_columns_len;
    size_t full_columns_cap;
    ColorPair *character_final_color_map;
    size_t map_len;
    Gradient rain_gradient;
    int64_t column_delay;
    int64_t resolve_delay;
    bool final_frame_shown;
    bool rain_complete;
    Phase phase;
    double rain_start;
    FrameMemo *resolve_runs;  // (input symbol, final foreground) -> template scene
} Matrix;

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
    size_t n = strlen(symbol);
    char *copy = malloc(n + 1);
    memcpy(copy, symbol, n + 1);
    list->items[list->len++] = copy;
}

static void size_push(size_t **items, size_t *len, size_t *cap, size_t v) {
    if (*len == *cap) {
        size_t c = *cap ? *cap * 2 : 8;
        size_t *grown = realloc(*items, c * sizeof(size_t));
        if (!grown) {
            return;
        }
        *items = grown;
        *cap = c;
    }
    (*items)[(*len)++] = v;
}

static void size_remove_at(size_t *a, size_t *len, size_t idx) {
    if (idx + 1 < *len) {
        memmove(&a[idx], &a[idx + 1], (*len - idx - 1) * sizeof(size_t));
    }
    (*len)--;
}

static bool size_contains(const size_t *a, size_t len, size_t v) {
    for (size_t i = 0; i < len; i++) {
        if (a[i] == v) {
            return true;
        }
    }
    return false;
}

static void id_push(CharId **items, size_t *len, size_t *cap, CharId v) {
    if (*len == *cap) {
        size_t c = *cap ? *cap * 2 : 8;
        CharId *grown = realloc(*items, c * sizeof(CharId));
        if (!grown) {
            return;
        }
        *items = grown;
        *cap = c;
    }
    (*items)[(*len)++] = v;
}

static void id_remove_at(CharId *a, size_t *len, size_t idx) {
    if (idx + 1 < *len) {
        memmove(&a[idx], &a[idx + 1], (*len - idx - 1) * sizeof(CharId));
    }
    (*len)--;
}

static char *dup_cstr(const char *s) {
    if (!s) {
        s = "";
    }
    size_t n = strlen(s);
    char *out = malloc(n + 1);
    if (out) {
        memcpy(out, s, n + 1);
    }
    return out;
}

static Easing no_ease(void) {
    Easing e;
    memset(&e, 0, sizeof(e));
    return e;
}

void matrix_config_defaults(void *cfg_ptr) {
    MatrixConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    color_from_hex("dbffdb", &cfg->highlight_color);
    push_color_default(&cfg->rain_color_gradient, "92be92");
    push_color_default(&cfg->rain_color_gradient, "185318");
    static const char *rain_symbols[] = {
        "2", "5", "9", "8", "Z", "*", ")", ":", ".", "\"", "=", "+", "-", "¦", "|", "_",
        "ｦ", "ｱ", "ｳ", "ｴ", "ｵ", "ｶ", "ｷ", "ｹ", "ｺ", "ｻ", "ｼ", "ｽ", "ｾ", "ｿ", "ﾀ", "ﾂ",
        "ﾃ", "ﾅ", "ﾆ", "ﾇ", "ﾈ", "ﾊ", "ﾋ", "ﾎ", "ﾏ", "ﾐ", "ﾑ", "ﾒ", "ﾓ", "ﾔ", "ﾕ", "ﾗ",
        "ﾘ", "ﾜ",
    };
    for (size_t i = 0; i < sizeof(rain_symbols) / sizeof(rain_symbols[0]); i++) {
        push_symbol_default(&cfg->rain_symbols, rain_symbols[i]);
    }
    cfg->rain_fall_delay_range.start = 2;
    cfg->rain_fall_delay_range.end = 15;
    cfg->rain_column_delay_range.start = 3;
    cfg->rain_column_delay_range.end = 9;
    cfg->rain_time = 15;
    cfg->symbol_swap_chance = 0.005;
    cfg->color_swap_chance = 0.001;
    cfg->resolve_delay = 3;
    push_color_default(&cfg->final_gradient_stops, "92be92");
    push_color_default(&cfg->final_gradient_stops, "336b33");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_frames = 3;
    cfg->final_gradient_direction = GRADIENT_RADIAL;
}

void matrix_free_config(void *cfg_ptr) {
    MatrixConfig *cfg = cfg_ptr;
    free(cfg->rain_color_gradient.items);
    for (size_t i = 0; i < cfg->rain_symbols.len; i++) {
        free(cfg->rain_symbols.items[i]);
    }
    free(cfg->rain_symbols.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

// Animation.set_appearance shorthand (upstream character.animation.set_appearance).
static void matrix_set_appearance(EngineCtx *ctx, CharId id, const char *symbol, const ColorPair *colors) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    bool uses_pre = ch->uses_input_preexisting_colors;
    animation_set_appearance(&ch->animation, uses_pre, symbol, colors);
}

// MatrixIterator._has_input_colors.
static bool matrix_has_input_colors(EngineCtx *ctx, CharId character) {
    Animation *anim = &ctx->terminal.arena.items[character].animation;
    return anim->has_input_fg || anim->has_input_bg;
}

// RainColumn.setup_column.
static void rain_setup_column(RainColumn *rc, EngineCtx *ctx, const MatrixConfig *cfg, ColumnPhase phase) {
    rc->pending_len = 0;
    rc->phase = phase;
    for (size_t i = 0; i < rc->characters_len; i++) {
        CharId id = rc->characters[i];
        terminal_set_character_visibility(&ctx->terminal, id, false);
        id_push(&rc->pending, &rc->pending_len, &rc->pending_cap, id);
        motion_set_coordinate(&ctx->terminal.arena.items[id].motion,
                              ctx->terminal.arena.items[id].input_coord);
    }
    rc->visible_len = 0;
    if (rc->phase == COLUMN_FILL) {
        int64_t lo = py_floor_div(cfg->rain_fall_delay_range.start, 3);
        int64_t hi = py_floor_div(cfg->rain_fall_delay_range.end, 3);
        if (lo < 1) {
            lo = 1;
        }
        if (hi < 1) {
            hi = 1;
        }
        rc->base_rain_fall_delay = rng_randint(&ctx->rng, lo, hi);
    } else {
        rc->base_rain_fall_delay =
            rng_randint(&ctx->rng, cfg->rain_fall_delay_range.start, cfg->rain_fall_delay_range.end);
    }
    rc->active_rain_fall_delay = 0;
    if (rc->phase == COLUMN_RAIN) {
        int64_t lo = (int64_t)((double)rc->characters_len * 0.1);
        if (lo < 1) {
            lo = 1;
        }
        int64_t hi = (int64_t)rc->characters_len;
        rc->length = (size_t)rng_randint(&ctx->rng, lo, hi);
    } else {
        rc->length = rc->characters_len;
    }
    rc->hold_time = 0;
    if (rc->length == rc->characters_len) {
        rc->hold_time = rng_randint(&ctx->rng, 20, 45);
    }
}

// RainColumn.fade_last_character.
static void rain_fade_last_character(RainColumn *rc, EngineCtx *ctx, const Color *rain_colors, size_t rain_colors_len) {
    size_t tail_len = rain_colors_len < 3 ? rain_colors_len : 3;
    const Color *tail = rain_colors + (rain_colors_len - tail_len);
    size_t idx = rng_choice_index(&ctx->rng, tail_len);
    Color darker_color = color_adjust_brightness(&tail[idx], 0.65);
    CharId target = rc->visible[0];
    char *symbol = dup_cstr(ctx->terminal.arena.items[target].animation.current_visual->symbol);
    ColorPair colors;
    memset(&colors, 0, sizeof(colors));
    colors.has_fg = true;
    colors.fg = darker_color;
    matrix_set_appearance(ctx, target, symbol, &colors);
    free(symbol);
}

// RainColumn.trim_column.
static void rain_trim_column(RainColumn *rc, EngineCtx *ctx, const Color *rain_colors, size_t rain_colors_len) {
    if (rc->visible_len == 0) {
        return;
    }
    CharId popped_char = rc->visible[0];
    id_remove_at(rc->visible, &rc->visible_len, 0);
    terminal_set_character_visibility(&ctx->terminal, popped_char, false);
    if (rc->visible_len > 1) {
        rain_fade_last_character(rc, ctx, rain_colors, rain_colors_len);
    }
}

// RainColumn.drop_column.
static void rain_drop_column(RainColumn *rc, EngineCtx *ctx) {
    int64_t canvas_bottom = ctx->terminal.canvas.bottom;
    size_t keep = 0;
    for (size_t i = 0; i < rc->visible_len; i++) {
        CharId character = rc->visible[i];
        Motion *motion = &ctx->terminal.arena.items[character].motion;
        Coord current = motion->current_coord;
        motion_set_coordinate(motion, coord_new(current.column, current.row - 1));
        if (motion->current_coord.row < canvas_bottom) {
            terminal_set_character_visibility(&ctx->terminal, character, false);
        } else {
            rc->visible[keep++] = character;
        }
    }
    rc->visible_len = keep;
}

// RainColumn.resolve_char.
static CharId rain_resolve_char(RainColumn *rc, EngineCtx *ctx) {
    int64_t index = rng_randint(&ctx->rng, 0, (int64_t)rc->visible_len - 1);
    CharId id = rc->visible[index];
    id_remove_at(rc->visible, &rc->visible_len, (size_t)index);
    return id;
}

// RainColumn.tick.
static void rain_tick(RainColumn *rc, EngineCtx *ctx, const MatrixConfig *cfg, const Color *rain_colors,
                      size_t rain_colors_len) {
    if (rc->active_rain_fall_delay == 0) {
        if (rc->pending_len > 0) {
            CharId next_char = rc->pending[0];
            id_remove_at(rc->pending, &rc->pending_len, 0);
            const char *symbol = cfg->rain_symbols.items[rng_choice_index(&ctx->rng, cfg->rain_symbols.len)];
            ColorPair highlight;
            memset(&highlight, 0, sizeof(highlight));
            highlight.has_fg = true;
            highlight.fg = cfg->highlight_color;
            matrix_set_appearance(ctx, next_char, symbol, &highlight);

            if (rc->visible_len > 0) {
                CharId previous_character = rc->visible[rc->visible_len - 1];
                char *prev_symbol =
                    dup_cstr(ctx->terminal.arena.items[previous_character].animation.current_visual->symbol);
                Color fg = rain_colors[rng_choice_index(&ctx->rng, rain_colors_len)];
                ColorPair colors;
                memset(&colors, 0, sizeof(colors));
                colors.has_fg = true;
                colors.fg = fg;
                matrix_set_appearance(ctx, previous_character, prev_symbol, &colors);
                free(prev_symbol);
            }
            terminal_set_character_visibility(&ctx->terminal, next_char, true);
            id_push(&rc->visible, &rc->visible_len, &rc->visible_cap, next_char);
        } else if (rc->visible_len > 0) {
            CharId last_char = rc->visible[rc->visible_len - 1];
            CharacterVisual *visual = ctx->terminal.arena.items[last_char].animation.current_visual;
            bool last_is_highlight = visual->has_colors && visual->colors.has_fg &&
                                     color_eq(&visual->colors.fg, &cfg->highlight_color);
            if (last_is_highlight) {
                char *symbol = dup_cstr(visual->symbol);
                Color fg = rain_colors[rng_choice_index(&ctx->rng, rain_colors_len)];
                ColorPair colors;
                memset(&colors, 0, sizeof(colors));
                colors.has_fg = true;
                colors.fg = fg;
                matrix_set_appearance(ctx, last_char, symbol, &colors);
                free(symbol);
            }

            if (rc->hold_time != 0) {
                rc->hold_time -= 1;
            } else if (rc->phase == COLUMN_RAIN) {
                if (rng_random(&ctx->rng) < rc->column_drop_chance) {
                    rain_drop_column(rc, ctx);
                }
                rain_trim_column(rc, ctx, rain_colors, rain_colors_len);
            }
        }

        if (rc->visible_len > rc->length) {
            rain_trim_column(rc, ctx, rain_colors, rain_colors_len);
        }
        rc->active_rain_fall_delay = rc->base_rain_fall_delay;
    } else {
        rc->active_rain_fall_delay -= 1;
    }

    for (size_t i = 0; i < rc->visible_len; i++) {
        CharId character = rc->visible[i];
        bool has_next_symbol = false;
        bool has_next_color = false;
        const char *next_symbol = NULL;
        Color next_color;
        memset(&next_color, 0, sizeof(next_color));

        if (rng_random(&ctx->rng) < cfg->symbol_swap_chance) {
            next_symbol = cfg->rain_symbols.items[rng_choice_index(&ctx->rng, cfg->rain_symbols.len)];
            has_next_symbol = true;
        }
        if (rng_random(&ctx->rng) < cfg->color_swap_chance) {
            next_color = rain_colors[rng_choice_index(&ctx->rng, rain_colors_len)];
            has_next_color = true;
        }
        if (!has_next_symbol && !has_next_color) {
            continue;
        }

        CharacterVisual *visual = ctx->terminal.arena.items[character].animation.current_visual;
        bool symbol_unchanged = !has_next_symbol || (visual->symbol && strcmp(next_symbol, visual->symbol) == 0);
        bool color_unchanged =
            !has_next_color ||
            (visual->has_colors && visual->colors.has_fg && color_eq(&visual->colors.fg, &next_color));
        if (symbol_unchanged && color_unchanged) {
            continue;
        }

        if (has_next_symbol && has_next_color) {
            ColorPair colors;
            memset(&colors, 0, sizeof(colors));
            colors.has_fg = true;
            colors.fg = next_color;
            matrix_set_appearance(ctx, character, next_symbol, &colors);
        } else if (has_next_symbol) {
            CharacterVisual *v = ctx->terminal.arena.items[character].animation.current_visual;
            ColorPair colors;
            memset(&colors, 0, sizeof(colors));
            if (v->has_colors && v->colors.has_fg) {
                colors.has_fg = true;
                colors.fg = v->colors.fg;
            }
            matrix_set_appearance(ctx, character, next_symbol, &colors);
        } else {
            CharacterVisual *v = ctx->terminal.arena.items[character].animation.current_visual;
            char *symbol = dup_cstr(v->symbol);
            ColorPair colors;
            memset(&colors, 0, sizeof(colors));
            colors.has_fg = true;
            colors.fg = next_color;
            matrix_set_appearance(ctx, character, symbol, &colors);
            free(symbol);
        }
    }
}

// Matrix::column_phase_for.
static ColumnPhase column_phase_for(Phase phase) {
    return phase == PHASE_FILL ? COLUMN_FILL : COLUMN_RAIN;
}

static bool matrix_all_active_full(const Matrix *st) {
    for (size_t i = 0; i < st->active_columns_len; i++) {
        size_t ci = st->active_columns[i];
        if (st->columns[ci].pending_len != 0 || st->columns[ci].phase != COLUMN_FILL) {
            return false;
        }
    }
    return true;
}

static int matrix_build(Effect *self, EngineCtx *ctx) {
    Matrix *st = self->state;
    MatrixConfig *cfg = &st->config;

    // __init__: rain_colors gradient (no RNG consumed).
    if (gradient_with_steps(cfg->rain_color_gradient.items, cfg->rain_color_gradient.len, 6, false,
                            &st->rain_gradient) != 0) {
        return -1;
    }

    Gradient final_gradient;
    if (gradient_new(cfg->final_gradient_stops.items, cfg->final_gradient_stops.len, cfg->final_gradient_steps.items,
                     cfg->final_gradient_steps.len, false, false, &final_gradient) != 0) {
        return -1;
    }
    CoordColorMap final_gradient_mapping;
    if (gradient_build_coordinate_color_mapping(&final_gradient, ctx->terminal.canvas.text_bottom,
                                                ctx->terminal.canvas.text_top, ctx->terminal.canvas.text_left,
                                                ctx->terminal.canvas.text_right, cfg->final_gradient_direction,
                                                &final_gradient_mapping) != 0) {
        gradient_free(&final_gradient);
        return -1;
    }

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    CharacterFilter filter = character_filter_default();
    size_t chars_len = 0;
    CharId *characters =
        terminal_get_characters(&ctx->terminal, &ctx->rng, filter, CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &chars_len);
    st->map_len = ctx->terminal.arena.len;
    st->character_final_color_map = calloc(st->map_len ? st->map_len : 1, sizeof(ColorPair));

    Easing ease = no_ease();
    int rc = 0;
    for (size_t i = 0; i < chars_len && rc == 0; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        char *input_symbol = dup_cstr(ch->input_symbol);
        Coord input_coord = ch->input_coord;
        bool has_input_fg = ch->animation.has_input_fg;
        Color input_fg = ch->animation.input_fg_color;
        bool has_input_bg = ch->animation.has_input_bg;
        Color input_bg = ch->animation.input_bg_color;
        bool uses_pre = ch->uses_input_preexisting_colors;

        ColorPair final_colors;
        memset(&final_colors, 0, sizeof(final_colors));
        if (dynamic) {
            final_colors.has_fg = has_input_fg;
            final_colors.fg = input_fg;
            final_colors.has_bg = has_input_bg;
            final_colors.bg = input_bg;
        } else {
            const Color *mapped = coordcolormap_get(&final_gradient_mapping, input_coord);
            final_colors.has_fg = true;
            if (mapped) {
                final_colors.fg = *mapped;
            }
        }
        if ((size_t)id < st->map_len) {
            st->character_final_color_map[id] = final_colors;
        }
        Color final_fg_color = final_colors.fg;
        Color final_bg_color = final_colors.bg;

        animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false, SYNC_DISTANCE, false, ease,
                            "resolve", uses_pre);
        Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "resolve");

        if (dynamic) {
            Gradient fg_gradient;
            Gradient bg_gradient;
            bool has_fg_gradient = false;
            bool has_bg_gradient = false;
            if (final_colors.has_fg) {
                Color stops[2] = {cfg->highlight_color, final_fg_color};
                if (gradient_with_steps(stops, 2, 8, false, &fg_gradient) == 0) {
                    has_fg_gradient = true;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0 && final_colors.has_bg) {
                Color stops[2] = {cfg->highlight_color, final_bg_color};
                if (gradient_with_steps(stops, 2, 8, false, &bg_gradient) == 0) {
                    has_bg_gradient = true;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0) {
                if (has_fg_gradient || has_bg_gradient) {
                    const char *syms[1] = {input_symbol};
                    if (!scene || scene_apply_gradient_to_symbols(scene, syms, 1, cfg->final_gradient_frames,
                                                                  has_fg_gradient ? &fg_gradient : NULL,
                                                                  has_bg_gradient ? &bg_gradient : NULL) != 0) {
                        rc = -1;
                    }
                } else {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    if (!scene || scene_add_frame(scene, input_symbol, cfg->final_gradient_frames, &vp) != 0) {
                        rc = -1;
                    }
                }
            }
            if (has_fg_gradient) {
                gradient_free(&fg_gradient);
            }
            if (has_bg_gradient) {
                gradient_free(&bg_gradient);
            }
        } else {
            // matrix.asm's visual_run_find: the resolve frames depend only on
            // the symbol and final color, so build them once for each pair.
            bool shareable = scene && !scene->has_preexisting_colors && !scene->preexisting_bold;
            Scene *template = shareable ? framemo_get(st->resolve_runs, input_symbol, final_fg_color) : NULL;
            if (template) {
                if (scene_append_frames(scene, template) != 0) {
                    rc = -1;
                }
            } else {
                Color stops[2] = {cfg->highlight_color, final_fg_color};
                Gradient resolve_gradient;
                if (gradient_with_steps(stops, 2, 8, false, &resolve_gradient) != 0) {
                    rc = -1;
                } else {
                    for (size_t j = 0; j < resolve_gradient.len && rc == 0; j++) {
                        VisualParams vp;
                        memset(&vp, 0, sizeof(vp));
                        vp.has_colors = true;
                        vp.colors.has_fg = true;
                        vp.colors.fg = resolve_gradient.spectrum[j];
                        if (!scene || scene_add_frame(scene, input_symbol, cfg->final_gradient_frames, &vp) != 0) {
                            rc = -1;
                        }
                    }
                    if (rc == 0 && shareable) {
                        (void)framemo_put(st->resolve_runs, input_symbol, final_fg_color, scene);
                    }
                    gradient_free(&resolve_gradient);
                }
            }
        }
        free(input_symbol);
    }
    free(characters);
    coordcolormap_free(&final_gradient_mapping);
    gradient_free(&final_gradient);
    if (rc != 0) {
        return -1;
    }

    CharacterFilter all_chars_filter;
    all_chars_filter.input_chars = true;
    all_chars_filter.inner_fill_chars = true;
    all_chars_filter.outer_fill_chars = true;
    all_chars_filter.added_chars = false;
    CharIdGrouping groups =
        terminal_get_characters_grouped(&ctx->terminal, all_chars_filter, CG_COLUMN_LEFT_TO_RIGHT);
    for (size_t g = 0; g < groups.len; g++) {
        CharId *column_chars = groups.buckets[g].items;
        size_t column_len = groups.buckets[g].len;
        for (size_t i = 0; i < column_len / 2; i++) {
            CharId tmp = column_chars[i];
            column_chars[i] = column_chars[column_len - 1 - i];
            column_chars[column_len - 1 - i] = tmp;
        }
        RainColumn column;
        memset(&column, 0, sizeof(column));
        column.characters = column_chars;
        column.characters_len = column_len;
        column.column_drop_chance = 0.08;
        rain_setup_column(&column, ctx, cfg, COLUMN_RAIN);
        if (st->columns_len == st->columns_cap) {
            size_t cap = st->columns_cap ? st->columns_cap * 2 : 8;
            RainColumn *grown = realloc(st->columns, cap * sizeof(RainColumn));
            if (!grown) {
                free(groups.buckets);
                return -1;
            }
            st->columns = grown;
            st->columns_cap = cap;
        }
        size_t column_index = st->columns_len;
        st->columns[st->columns_len++] = column;
        size_push(&st->pending_columns, &st->pending_columns_len, &st->pending_columns_cap, column_index);
    }
    free(groups.buckets);
    rng_shuffle(&ctx->rng, st->pending_columns, st->pending_columns_len, sizeof(size_t));

    // MatrixIterator.__init__: rain_start = time.time() (after build).
    st->rain_start = clock_now_wall(&ctx->clock);
    return 0;
}

static const char *matrix_next_frame(Effect *self, EngineCtx *ctx) {
    Matrix *st = self->state;
    MatrixConfig *cfg = &st->config;

    if (st->phase == PHASE_RAIN || st->phase == PHASE_FILL) {
        if (st->column_delay == 0) {
            if (st->phase == PHASE_RAIN) {
                int64_t draws = rng_randint(&ctx->rng, 1, 3);
                for (int64_t i = 0; i < draws; i++) {
                    if (st->pending_columns_len > 0) {
                        size_t ci = st->pending_columns[0];
                        size_remove_at(st->pending_columns, &st->pending_columns_len, 0);
                        size_push(&st->active_columns, &st->active_columns_len, &st->active_columns_cap, ci);
                    }
                }
            } else {
                while (st->pending_columns_len > 0) {
                    size_t ci = st->pending_columns[0];
                    size_remove_at(st->pending_columns, &st->pending_columns_len, 0);
                    size_push(&st->active_columns, &st->active_columns_len, &st->active_columns_cap, ci);
                }
            }
            if (st->phase == PHASE_RAIN) {
                st->column_delay = rng_randint(&ctx->rng, cfg->rain_column_delay_range.start,
                                               cfg->rain_column_delay_range.end);
            } else {
                st->column_delay = 1;
            }
        } else {
            st->column_delay -= 1;
        }

        size_t active_len = st->active_columns_len;
        size_t *active_snapshot = malloc((active_len ? active_len : 1) * sizeof(size_t));
        if (active_len) {
            memcpy(active_snapshot, st->active_columns, active_len * sizeof(size_t));
        }
        for (size_t k = 0; k < active_len; k++) {
            size_t column_index = active_snapshot[k];
            rain_tick(&st->columns[column_index], ctx, cfg, st->rain_gradient.spectrum, st->rain_gradient.len);

            if (st->columns[column_index].pending_len == 0) {
                if (st->columns[column_index].phase == COLUMN_FILL &&
                    !size_contains(st->full_columns, st->full_columns_len, column_index)) {
                    size_push(&st->full_columns, &st->full_columns_len, &st->full_columns_cap, column_index);
                } else if (st->columns[column_index].visible_len == 0) {
                    ColumnPhase column_phase = column_phase_for(st->phase);
                    rain_setup_column(&st->columns[column_index], ctx, cfg, column_phase);
                    size_push(&st->pending_columns, &st->pending_columns_len, &st->pending_columns_cap,
                              column_index);
                }
            }
        }
        free(active_snapshot);

        {
            size_t keep = 0;
            for (size_t i = 0; i < st->active_columns_len; i++) {
                if (st->columns[st->active_columns[i]].visible_len > 0) {
                    st->active_columns[keep++] = st->active_columns[i];
                }
            }
            st->active_columns_len = keep;
        }

        if (st->phase == PHASE_FILL && st->pending_columns_len == 0 && matrix_all_active_full(st)) {
            st->phase = PHASE_RESOLVE;
            st->active_columns_len = 0;
        }

        // effect_matrix.py:549 — time.time() rain deadline check.
        if (st->phase == PHASE_RAIN && cfg->rain_time > 0 &&
            clock_now_wall(&ctx->clock) - st->rain_start > (double)cfg->rain_time) {
            st->rain_complete = true;
            st->phase = PHASE_FILL;
            for (size_t i = 0; i < st->active_columns_len; i++) {
                size_t ci = st->active_columns[i];
                st->columns[ci].hold_time = 0;
                st->columns[ci].column_drop_chance = 1.0;
            }
            size_t pending_len = st->pending_columns_len;
            size_t *pending_snapshot = malloc((pending_len ? pending_len : 1) * sizeof(size_t));
            if (pending_len) {
                memcpy(pending_snapshot, st->pending_columns, pending_len * sizeof(size_t));
            }
            for (size_t i = 0; i < pending_len; i++) {
                rain_setup_column(&st->columns[pending_snapshot[i]], ctx, cfg, COLUMN_FILL);
            }
            free(pending_snapshot);
        }
    } else if (st->phase == PHASE_RESOLVE) {
        size_t full_len = st->full_columns_len;
        size_t *full_snapshot = malloc((full_len ? full_len : 1) * sizeof(size_t));
        if (full_len) {
            memcpy(full_snapshot, st->full_columns, full_len * sizeof(size_t));
        }
        for (size_t k = 0; k < full_len; k++) {
            size_t column_index = full_snapshot[k];
            rain_tick(&st->columns[column_index], ctx, cfg, st->rain_gradient.spectrum, st->rain_gradient.len);
            if (st->columns[column_index].visible_len > 0) {
                if (st->resolve_delay == 0) {
                    int64_t draws = rng_randint(&ctx->rng, 1, 4);
                    for (int64_t j = 0; j < draws; j++) {
                        if (st->columns[column_index].visible_len > 0) {
                            CharId next_char = rain_resolve_char(&st->columns[column_index], ctx);
                            const char *input_symbol = ctx->terminal.arena.items[next_char].input_symbol;
                            if (strcmp(input_symbol, " ") != 0 || matrix_has_input_colors(ctx, next_char)) {
                                engine_activate_scene(ctx, self, next_char, "resolve");
                                ac_insert(&ctx->active_characters, next_char);
                            } else {
                                terminal_set_character_visibility(&ctx->terminal, next_char, false);
                            }
                        }
                    }
                    st->resolve_delay = cfg->resolve_delay;
                } else {
                    st->resolve_delay -= 1;
                }
            }
        }
        free(full_snapshot);

        size_t keep = 0;
        for (size_t i = 0; i < st->full_columns_len; i++) {
            if (st->columns[st->full_columns[i]].visible_len > 0) {
                st->full_columns[keep++] = st->full_columns[i];
            }
        }
        st->full_columns_len = keep;
    }

    if (st->full_columns_len > 0 || st->active_columns_len > 0 || !ac_is_empty(&ctx->active_characters) ||
        st->pending_columns_len > 0 || !st->rain_complete) {
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    if (!st->final_frame_shown) {
        st->final_frame_shown = true;
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void matrix_destroy(Effect *self) {
    Matrix *st = self->state;
    if (!st) {
        return;
    }
    for (size_t i = 0; i < st->columns_len; i++) {
        free(st->columns[i].characters);
        free(st->columns[i].pending);
        free(st->columns[i].visible);
    }
    free(st->columns);
    free(st->pending_columns);
    free(st->active_columns);
    free(st->full_columns);
    free(st->character_final_color_map);
    framemo_free(st->resolve_runs);
    gradient_free(&st->rain_gradient);
    free(st);
    free(self);
}

static const EffectOps MATRIX_OPS = {matrix_build, matrix_next_frame, matrix_destroy, NULL};

Effect *matrix_make(const void *cfg) {
    Matrix *st = calloc(1, sizeof(Matrix));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const MatrixConfig *)cfg;
    st->resolve_runs = framemo_new();
    st->resolve_delay = st->config.resolve_delay;
    st->phase = PHASE_RAIN;
    effect->ops = &MATRIX_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec matrix_specs[] = {
    EF_SPEC("highlight-color", 0, EF_COLOR, offsetof(MatrixConfig, highlight_color)),
    EF_SPEC("rain-color-gradient", 0, EF_COLOR_LIST, offsetof(MatrixConfig, rain_color_gradient)),
    {"rain-symbols", 0, EF_STRING_LIST, offsetof(MatrixConfig, rain_symbols), NULL},
    EF_SPEC("rain-fall-delay-range", 0, EF_INT_RANGE, offsetof(MatrixConfig, rain_fall_delay_range)),
    EF_SPEC("rain-column-delay-range", 0, EF_INT_RANGE, offsetof(MatrixConfig, rain_column_delay_range)),
    EF_SPEC("rain-time", 0, EF_POS_INT, offsetof(MatrixConfig, rain_time)),
    EF_SPEC("symbol-swap-chance", 0, EF_FLOAT_POS, offsetof(MatrixConfig, symbol_swap_chance)),
    EF_SPEC("color-swap-chance", 0, EF_FLOAT_POS, offsetof(MatrixConfig, color_swap_chance)),
    EF_SPEC("resolve-delay", 0, EF_POS_INT, offsetof(MatrixConfig, resolve_delay)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(MatrixConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(MatrixConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-frames", 0, EF_POS_INT, offsetof(MatrixConfig, final_gradient_frames)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(MatrixConfig, final_gradient_direction)),
};

const EffectEntry matrix_entry = {
    "matrix",
    matrix_specs,
    sizeof(matrix_specs) / sizeof(matrix_specs[0]),
    sizeof(MatrixConfig),
    matrix_config_defaults,
    matrix_free_config,
    matrix_make,
};
