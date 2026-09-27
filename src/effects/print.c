#include "effects/print.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/motion.h"
#include "engine/terminal.h"
#include "utils/graphics.h"

typedef struct {
    CharId *untyped;
    size_t untyped_len;
    size_t untyped_cap;
    CharId *typed;
    size_t typed_len;
    size_t typed_cap;
} PrintRow;

typedef struct {
    PrintConfig config;
    PrintRow *pending_rows;
    size_t pending_len;
    size_t pending_cap;
    size_t pending_head;
    PrintRow *processed_rows;
    size_t processed_len;
    size_t processed_cap;
    CharId typing_head;
    ColorPair *final_colors;
    bool *final_colors_present;
    size_t final_colors_len;
    PrintRow current_row;
    bool typing;
    int64_t last_column;
    FrameMemo *head_runs;  // (symbol, final fg) -> template head scene
} Print;

#define SET_INVISIBLE_CALLBACK 0

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

void print_config_defaults(void *cfg_ptr) {
    PrintConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->print_head_return_speed = 1.5;
    cfg->print_speed = 2;
    easing_parse("in_out_quad", &cfg->print_head_easing);
    push_color_default(&cfg->final_gradient_stops, "02b8bd");
    push_color_default(&cfg->final_gradient_stops, "c1f0e3");
    push_color_default(&cfg->final_gradient_stops, "00ffa0");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_DIAGONAL;
}

void print_free_config(void *cfg_ptr) {
    PrintConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static void row_push_untyped(PrintRow *row, CharId id) {
    if (row->untyped_len == row->untyped_cap) {
        size_t cap = row->untyped_cap ? row->untyped_cap * 2 : 8;
        row->untyped = realloc(row->untyped, cap * sizeof(CharId));
        row->untyped_cap = cap;
    }
    row->untyped[row->untyped_len++] = id;
}

static void row_push_typed(PrintRow *row, CharId id) {
    if (row->typed_len == row->typed_cap) {
        size_t cap = row->typed_cap ? row->typed_cap * 2 : 8;
        row->typed = realloc(row->typed, cap * sizeof(CharId));
        row->typed_cap = cap;
    }
    row->typed[row->typed_len++] = id;
}

static bool row_type_char(PrintRow *row, CharId *out) {
    if (row->untyped_len == 0) {
        return false;
    }
    *out = row->untyped[0];
    memmove(row->untyped, row->untyped + 1, (row->untyped_len - 1) * sizeof(CharId));
    row->untyped_len--;
    row_push_typed(row, *out);
    return true;
}

static void row_move_up(PrintRow *row, EngineCtx *ctx) {
    for (size_t i = 0; i < row->typed_len; i++) {
        Motion *motion = &ctx->terminal.arena.items[row->typed[i]].motion;
        Coord current = motion->current_coord;
        motion_set_coordinate(motion, coord_new(current.column, current.row + 1));
    }
}

static bool ids_all_fill(const EngineCtx *ctx, const CharId *ids, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!ctx->terminal.arena.items[ids[i]].is_fill_character) {
            return false;
        }
    }
    return true;
}

static void row_free(PrintRow *row) {
    free(row->untyped);
    free(row->typed);
    memset(row, 0, sizeof(*row));
}

static int print_make_row(Effect *self, EngineCtx *ctx, const CharId *chars, size_t n, PrintRow *out) {
    Print *st = self->state;
    memset(out, 0, sizeof(*out));
    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    Color typing_head_color;
    color_from_hex("ffffff", &typing_head_color);

    bool all_spaces = true;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(ctx->terminal.arena.items[chars[i]].input_symbol, " ") != 0) {
            all_spaces = false;
            break;
        }
    }
    size_t limit = n;
    int64_t right_extent = INT64_MAX;
    if (all_spaces) {
        limit = n > 0 ? 1 : 0;
    } else {
        right_extent = INT64_MIN;
        for (size_t i = 0; i < n; i++) {
            EffectCharacter *ch = &ctx->terminal.arena.items[chars[i]];
            if (!ch->is_fill_character && ch->input_coord.column > right_extent) {
                right_extent = ch->input_coord.column;
            }
        }
    }

    int rc = 0;
    for (size_t i = 0; i < limit && rc == 0; i++) {
        CharId id = chars[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        if (!all_spaces && ch->input_coord.column > right_extent) {
            continue;
        }
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);
        int64_t input_column = ch->input_coord.column;
        bool uses_pre = ch->uses_input_preexisting_colors;

        motion_set_coordinate(&ch->motion, coord_new(input_column, 1));
        const char *ta = animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, (Easing){0}, "",
                                             uses_pre);
        char typed_animation[32];
        typed_animation[0] = '\0';
        if (ta) {
            strncpy(typed_animation, ta, sizeof(typed_animation) - 1);
            typed_animation[sizeof(typed_animation) - 1] = '\0';
        }
        Scene *scene = (Scene *)om_get(&ch->animation.scenes, typed_animation);
        const char *head_symbols[5] = {"█", "▓", "▒", "░", input_symbol};

        if (dynamic) {
            ColorPair final_colors;
            memset(&final_colors, 0, sizeof(final_colors));
            if ((size_t)id < st->final_colors_len && st->final_colors_present[id]) {
                final_colors = st->final_colors[id];
            }
            Gradient fg_gradient;
            Gradient bg_gradient;
            bool has_fg = false;
            bool has_bg = false;
            if (final_colors.has_fg) {
                Color stops[2] = {typing_head_color, final_colors.fg};
                if (gradient_with_steps(stops, 2, 5, false, &fg_gradient) == 0) {
                    has_fg = true;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0 && final_colors.has_bg) {
                Color stops[2] = {typing_head_color, final_colors.bg};
                if (gradient_with_steps(stops, 2, 5, false, &bg_gradient) == 0) {
                    has_bg = true;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0) {
                if (has_fg || has_bg) {
                    if (scene_apply_gradient_to_symbols(scene, head_symbols, 5, 3, has_fg ? &fg_gradient : NULL,
                                                        has_bg ? &bg_gradient : NULL) != 0) {
                        rc = -1;
                    }
                } else {
                    Gradient head_gradient;
                    if (gradient_with_steps((Color[2]){typing_head_color, typing_head_color}, 2, 4, false,
                                            &head_gradient) != 0) {
                        rc = -1;
                    } else {
                        if (scene_apply_gradient_to_symbols(scene, head_symbols, 4, 3, &head_gradient, NULL) != 0) {
                            rc = -1;
                        }
                        gradient_free(&head_gradient);
                    }
                    if (rc == 0) {
                        VisualParams vp;
                        memset(&vp, 0, sizeof(vp));
                        vp.has_colors = true;
                        if (scene_add_frame(scene, input_symbol, 3, &vp) != 0) {
                            rc = -1;
                        }
                    }
                }
            }
            if (has_fg) {
                gradient_free(&fg_gradient);
            }
            if (has_bg) {
                gradient_free(&bg_gradient);
            }
        } else {
            Color final_fg;
            memset(&final_fg, 0, sizeof(final_fg));
            if ((size_t)id < st->final_colors_len && st->final_colors_present[id]) {
                final_fg = st->final_colors[id].fg;
            }
            // The head frames depend on the input symbol and final color only,
            // so later characters with the same pair copy the first one's
            // scene (the asm engine's visual_run_find/keep memo).
            bool shareable = scene && !scene->has_preexisting_colors && !scene->preexisting_bold;
            Scene *tmpl = shareable ? framemo_get(st->head_runs, input_symbol, final_fg) : NULL;
            if (tmpl) {
                if (scene_append_frames(scene, tmpl) != 0) {
                    rc = -1;
                }
            } else {
                Color stops[2] = {typing_head_color, final_fg};
                Gradient color_gradient;
                if (gradient_with_steps(stops, 2, 5, false, &color_gradient) != 0) {
                    rc = -1;
                } else {
                    if (scene_apply_gradient_to_symbols(scene, head_symbols, 5, 3, &color_gradient, NULL) != 0) {
                        rc = -1;
                    } else if (shareable && framemo_put(st->head_runs, input_symbol, final_fg, scene) != 0) {
                        rc = -1;
                    }
                    gradient_free(&color_gradient);
                }
            }
        }

        if (rc == 0) {
            engine_activate_scene(ctx, self, id, typed_animation);
            row_push_untyped(out, id);
        }
        free(input_symbol);
    }
    return rc;
}

static int print_build(Effect *self, EngineCtx *ctx) {
    Print *st = self->state;
    PrintConfig *cfg = &st->config;

    st->typing_head = terminal_add_character(&ctx->terminal, "█", coord_new(1, 1));

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
    CharacterFilter fills_filter;
    fills_filter.input_chars = true;
    fills_filter.inner_fill_chars = true;
    fills_filter.outer_fill_chars = true;
    fills_filter.added_chars = false;

    size_t n = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, fills_filter,
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);
    size_t arena_len = ctx->terminal.arena.len;
    st->final_colors_len = arena_len;
    st->final_colors = calloc(arena_len ? arena_len : 1, sizeof(ColorPair));
    st->final_colors_present = calloc(arena_len ? arena_len : 1, sizeof(bool));
    for (size_t i = 0; i < n; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        ColorPair colors;
        memset(&colors, 0, sizeof(colors));
        if (dynamic) {
            colors.has_fg = ch->animation.has_input_fg;
            colors.fg = ch->animation.input_fg_color;
            colors.has_bg = ch->animation.has_input_bg;
            colors.bg = ch->animation.input_bg_color;
        } else {
            const Color *mapped = coordcolormap_get(&mapping, ch->input_coord);
            colors.has_fg = true;
            if (mapped) {
                colors.fg = *mapped;
            } else {
                color_from_hex("ffffff", &colors.fg);
            }
        }
        if ((size_t)id < st->final_colors_len) {
            st->final_colors[id] = colors;
            st->final_colors_present[id] = true;
        }
    }
    free(characters);

    CharIdGrouping input_rows =
        terminal_get_characters_grouped(&ctx->terminal, fills_filter, CG_ROW_TOP_TO_BOTTOM);
    int rc = 0;
    for (size_t r = 0; r < input_rows.len && rc == 0; r++) {
        PrintRow row;
        if (print_make_row(self, ctx, input_rows.buckets[r].items, input_rows.buckets[r].len, &row) != 0) {
            rc = -1;
            break;
        }
        if (st->pending_len == st->pending_cap) {
            size_t cap = st->pending_cap ? st->pending_cap * 2 : 8;
            st->pending_rows = realloc(st->pending_rows, cap * sizeof(PrintRow));
            st->pending_cap = cap;
        }
        st->pending_rows[st->pending_len++] = row;
    }
    charidgrouping_free(&input_rows);

    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    if (rc != 0) {
        return rc;
    }

    if (st->pending_len > 0) {
        st->current_row = st->pending_rows[0];
        st->pending_head = 1;
    } else {
        memset(&st->current_row, 0, sizeof(st->current_row));
        st->pending_head = 0;
    }
    st->typing = true;
    st->last_column = 0;
    return 0;
}

static const char *print_next_frame(Effect *self, EngineCtx *ctx) {
    Print *st = self->state;
    PrintConfig *cfg = &st->config;

    if (!ac_is_empty(&ctx->active_characters) || st->typing) {
        if (ctx->terminal.arena.items[st->typing_head].motion.active_path != NULL) {
            // print head is performing a carriage return
        } else if (st->current_row.untyped_len != 0) {
            int64_t count = (int64_t)st->current_row.untyped_len;
            if (count > cfg->print_speed) {
                count = cfg->print_speed;
            }
            for (int64_t i = 0; i < count; i++) {
                CharId next_char;
                if (row_type_char(&st->current_row, &next_char)) {
                    terminal_set_character_visibility(&ctx->terminal, next_char, true);
                    ac_insert(&ctx->active_characters, next_char);
                    st->last_column = ctx->terminal.arena.items[next_char].input_coord.column;
                }
            }
        } else {
            PrintRow finished = st->current_row;
            memset(&st->current_row, 0, sizeof(st->current_row));
            if (st->processed_len == st->processed_cap) {
                size_t cap = st->processed_cap ? st->processed_cap * 2 : 8;
                st->processed_rows = realloc(st->processed_rows, cap * sizeof(PrintRow));
                st->processed_cap = cap;
            }
            st->processed_rows[st->processed_len++] = finished;

            if (st->pending_head < st->pending_len) {
                for (size_t r = 0; r < st->processed_len; r++) {
                    row_move_up(&st->processed_rows[r], ctx);
                }
                st->current_row = st->pending_rows[st->pending_head++];
                bool last_row_all_fill =
                    ids_all_fill(ctx, st->processed_rows[st->processed_len - 1].typed,
                                 st->processed_rows[st->processed_len - 1].typed_len);
                bool current_row_all_fill =
                    ids_all_fill(ctx, st->current_row.untyped, st->current_row.untyped_len);
                if (!last_row_all_fill && !current_row_all_fill) {
                    int64_t left_extent = INT64_MAX;
                    for (size_t k = 0; k < st->current_row.untyped_len; k++) {
                        EffectCharacter *c = &ctx->terminal.arena.items[st->current_row.untyped[k]];
                        if (!c->is_fill_character && c->input_coord.column < left_extent) {
                            left_extent = c->input_coord.column;
                        }
                    }
                    int64_t text_right = ctx->terminal.canvas.text_right;
                    size_t keep = 0;
                    for (size_t k = 0; k < st->current_row.untyped_len; k++) {
                        int64_t column = ctx->terminal.arena.items[st->current_row.untyped[k]].input_coord.column;
                        if (left_extent <= column && column <= text_right) {
                            st->current_row.untyped[keep++] = st->current_row.untyped[k];
                        }
                    }
                    st->current_row.untyped_len = keep;
                }
                {
                    EffectCharacter *head = &ctx->terminal.arena.items[st->typing_head];
                    motion_set_coordinate(&head->motion, coord_new(st->last_column, 1));
                }
                terminal_set_character_visibility(&ctx->terminal, st->typing_head, true);
                int64_t target_column =
                    ctx->terminal.arena.items[st->current_row.untyped[0]].input_coord.column;
                EffectCharacter *head = &ctx->terminal.arena.items[st->typing_head];
                motion_clear_paths(&head->motion);
                if (motion_new_path(&head->motion, cfg->print_head_return_speed, true, cfg->print_head_easing,
                                    false, 0, 0, false, "carriage_return_path", NULL) == 0) {
                    Path *p = (Path *)om_get(&head->motion.paths, "carriage_return_path");
                    Waypoint wp;
                    memset(&wp, 0, sizeof(wp));
                    (void)path_new_waypoint(p, coord_new(target_column, 1), NULL, 0, "", &wp);
                    waypoint_free(&wp);
                }
                engine_activate_path(ctx, self, st->typing_head, "carriage_return_path");
                CallerKey caller;
                memset(&caller, 0, sizeof(caller));
                caller.kind = CALLER_PATH;
                caller.id = "carriage_return_path";
                EventAction action;
                memset(&action, 0, sizeof(action));
                action.kind = ACTION_CALLBACK;
                action.cb.id = SET_INVISIBLE_CALLBACK;
                (void)engine_register_event(ctx, st->typing_head, EVENT_PATH_COMPLETE, &caller, &action);
                ac_insert(&ctx->active_characters, st->typing_head);
            } else {
                st->typing = false;
            }
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void print_dispatch_callback(Effect *self, EngineCtx *ctx, CharId character, const EffectCallback *cb) {
    (void)self;
    if (cb->id == SET_INVISIBLE_CALLBACK) {
        terminal_set_character_visibility(&ctx->terminal, character, false);
    }
}

static void print_destroy(Effect *self) {
    Print *st = self->state;
    if (!st) {
        return;
    }
    framemo_free(st->head_runs);
    row_free(&st->current_row);
    for (size_t i = st->pending_head; i < st->pending_len; i++) {
        row_free(&st->pending_rows[i]);
    }
    free(st->pending_rows);
    for (size_t i = 0; i < st->processed_len; i++) {
        row_free(&st->processed_rows[i]);
    }
    free(st->processed_rows);
    free(st->final_colors);
    free(st->final_colors_present);
    free(st);
    free(self);
}

static const EffectOps PRINT_OPS = {print_build, print_next_frame, print_destroy, print_dispatch_callback};

Effect *print_make(const void *cfg) {
    Print *st = calloc(1, sizeof(Print));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const PrintConfig *)cfg;
    st->head_runs = framemo_new();
    effect->ops = &PRINT_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec print_specs[] = {
    EF_SPEC("print-head-return-speed", 0, EF_FLOAT_POS, offsetof(PrintConfig, print_head_return_speed)),
    EF_SPEC("print-speed", 0, EF_POS_INT, offsetof(PrintConfig, print_speed)),
    EF_SPEC("print-head-easing", 0, EF_EASING, offsetof(PrintConfig, print_head_easing)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(PrintConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(PrintConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(PrintConfig, final_gradient_direction)),
};

const EffectEntry print_entry = {
    "print",
    print_specs,
    sizeof(print_specs) / sizeof(print_specs[0]),
    sizeof(PrintConfig),
    print_config_defaults,
    print_free_config,
    print_make,
};
