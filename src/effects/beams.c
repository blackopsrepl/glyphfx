#include "effects/beams.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/graphics.h"

typedef enum {
    DIR_ROW,
    DIR_COLUMN,
} BeamDirection;

typedef enum {
    BEAMS_BEAMS,
    BEAMS_FINAL_WIPE,
    BEAMS_COMPLETE,
} BeamsPhase;

typedef struct {
    CharId *items;
    size_t len;
} BeamsIdVec;

typedef struct {
    BeamsIdVec characters;
    BeamDirection direction;
    double speed;
    double next_character_counter;
} BeamGroup;

typedef struct {
    BeamsConfig config;
    BeamGroup *pending_groups;
    size_t pending_len;
    size_t pending_cap;
    BeamGroup *active_groups;
    size_t active_len;
    size_t active_cap;
    ColorPair *final_colors;
    bool *final_present;
    size_t map_len;
    BeamsIdVec *final_wipe_groups;
    size_t final_wipe_len;
    size_t final_wipe_cap;
    int64_t delay;
    BeamsPhase phase;
} Beams;

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

void beams_config_defaults(void *cfg_ptr) {
    BeamsConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    push_symbol_default(&cfg->beam_row_symbols, "▂");
    push_symbol_default(&cfg->beam_row_symbols, "▁");
    push_symbol_default(&cfg->beam_row_symbols, "_");
    push_symbol_default(&cfg->beam_column_symbols, "▌");
    push_symbol_default(&cfg->beam_column_symbols, "▍");
    push_symbol_default(&cfg->beam_column_symbols, "▎");
    push_symbol_default(&cfg->beam_column_symbols, "▏");
    cfg->beam_delay = 6;
    cfg->beam_row_speed_range.start = 15;
    cfg->beam_row_speed_range.end = 60;
    cfg->beam_column_speed_range.start = 9;
    cfg->beam_column_speed_range.end = 15;
    push_color_default(&cfg->beam_gradient_stops, "ffffff");
    push_color_default(&cfg->beam_gradient_stops, "00D1FF");
    push_color_default(&cfg->beam_gradient_stops, "8A008A");
    push_int_default(&cfg->beam_gradient_steps, 2);
    push_int_default(&cfg->beam_gradient_steps, 6);
    cfg->beam_gradient_frames = 2;
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "ffffff");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_frames = 4;
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
    cfg->final_wipe_speed = 3;
}

void beams_free_config(void *cfg_ptr) {
    BeamsConfig *cfg = cfg_ptr;
    for (size_t i = 0; i < cfg->beam_row_symbols.len; i++) {
        free(cfg->beam_row_symbols.items[i]);
    }
    free(cfg->beam_row_symbols.items);
    for (size_t i = 0; i < cfg->beam_column_symbols.len; i++) {
        free(cfg->beam_column_symbols.items[i]);
    }
    free(cfg->beam_column_symbols.items);
    free(cfg->beam_gradient_stops.items);
    free(cfg->beam_gradient_steps.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static void group_free(BeamGroup *g) {
    free(g->characters.items);
    g->characters.items = NULL;
    g->characters.len = 0;
}

static void groups_free(BeamGroup *groups, size_t len) {
    for (size_t i = 0; i < len; i++) {
        group_free(&groups[i]);
    }
    free(groups);
}

static void groupvec_push(BeamGroup **arr, size_t *len, size_t *cap, BeamGroup g) {
    if (*len == *cap) {
        size_t next = *cap ? *cap * 2 : 8;
        BeamGroup *grown = realloc(*arr, next * sizeof(BeamGroup));
        if (!grown) {
            return;
        }
        *arr = grown;
        *cap = next;
    }
    (*arr)[(*len)++] = g;
}

static BeamGroup groupvec_remove0(BeamGroup *arr, size_t *len) {
    BeamGroup first = arr[0];
    for (size_t i = 1; i < *len; i++) {
        arr[i - 1] = arr[i];
    }
    (*len)--;
    return first;
}

static void idvec_reverse(CharId *items, size_t len) {
    for (size_t i = 0; i < len / 2; i++) {
        CharId tmp = items[i];
        items[i] = items[len - 1 - i];
        items[len - 1 - i] = tmp;
    }
}

static BeamGroup beams_make_group(EngineCtx *ctx, const BeamsConfig *cfg, BeamsIdVec chars, BeamDirection direction) {
    IntRange range = direction == DIR_ROW ? cfg->beam_row_speed_range : cfg->beam_column_speed_range;
    double speed = (double)rng_randint(&ctx->rng, range.start, range.end) * 0.1;
    for (size_t i = 1; i < chars.len; i++) {
        CharId key = chars.items[i];
        int64_t kk = direction == DIR_ROW ? ctx->terminal.arena.items[key].input_coord.column
                                          : ctx->terminal.arena.items[key].input_coord.row;
        size_t j = i;
        while (j > 0) {
            CharId prev = chars.items[j - 1];
            int64_t pk = direction == DIR_ROW ? ctx->terminal.arena.items[prev].input_coord.column
                                              : ctx->terminal.arena.items[prev].input_coord.row;
            if (pk <= kk) {
                break;
            }
            chars.items[j] = prev;
            j--;
        }
        chars.items[j] = key;
    }
    if (rng_choice_index(&ctx->rng, 2) == 0) {
        idvec_reverse(chars.items, chars.len);
    }
    BeamGroup g;
    g.characters = chars;
    g.direction = direction;
    g.speed = speed;
    g.next_character_counter = 0.0;
    return g;
}

static bool beams_get_next_character(EngineCtx *ctx, Effect *self, BeamGroup *group, CharId *out) {
    group->next_character_counter -= 1.0;
    CharId next = group->characters.items[0];
    for (size_t i = 1; i < group->characters.len; i++) {
        group->characters.items[i - 1] = group->characters.items[i];
    }
    group->characters.len--;

    const char *active_scene = ctx->terminal.arena.items[next].animation.active_scene;
    bool return_value;
    if (active_scene) {
        char name[128];
        strncpy(name, active_scene, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[next].animation.scenes, name);
        if (scene) {
            scene_reset(scene);
        }
        return_value = false;
    } else {
        terminal_set_character_visibility(&ctx->terminal, next, true);
        return_value = true;
    }
    const char *scene_name = group->direction == DIR_ROW ? "beam_row" : "beam_column";
    engine_activate_scene(ctx, self, next, scene_name);
    if (out) {
        *out = next;
    }
    return return_value;
}

static int gradient_pair(Color a, Color b, int64_t steps, Gradient *out) {
    Color stops[2] = {a, b};
    return gradient_with_steps(stops, 2, steps, false, out);
}

static int beams_build(Effect *self, EngineCtx *ctx) {
    Beams *st = self->state;
    BeamsConfig *cfg = &st->config;
    int rc = 0;

    CharIdGrouping wipe = terminal_get_characters_grouped(&ctx->terminal, character_filter_default(),
                                                          CG_DIAGONAL_TOP_LEFT_TO_BOTTOM_RIGHT);
    st->final_wipe_cap = wipe.len;
    st->final_wipe_len = wipe.len;
    st->final_wipe_groups = calloc(wipe.len ? wipe.len : 1, sizeof(BeamsIdVec));
    for (size_t i = 0; i < wipe.len; i++) {
        st->final_wipe_groups[i].items = wipe.buckets[i].items;
        st->final_wipe_groups[i].len = wipe.buckets[i].len;
    }
    free(wipe.buckets);

    Gradient final_gradient;
    memset(&final_gradient, 0, sizeof(final_gradient));
    if (gradient_new(cfg->final_gradient_stops.items, cfg->final_gradient_stops.len,
                     cfg->final_gradient_steps.items, cfg->final_gradient_steps.len, false, false,
                     &final_gradient) != 0) {
        return -1;
    }
    CoordColorMap mapping;
    memset(&mapping, 0, sizeof(mapping));
    if (gradient_build_coordinate_color_mapping(&final_gradient, ctx->terminal.canvas.text_bottom,
                                                ctx->terminal.canvas.text_top, ctx->terminal.canvas.text_left,
                                                ctx->terminal.canvas.text_right, cfg->final_gradient_direction,
                                                &mapping) != 0) {
        gradient_free(&final_gradient);
        return -1;
    }

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    CharacterFilter all_chars = {true, true, true, false};

    size_t characters_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, all_chars,
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &characters_len);
    st->map_len = ctx->terminal.arena.len;
    st->final_colors = calloc(st->map_len ? st->map_len : 1, sizeof(ColorPair));
    st->final_present = calloc(st->map_len ? st->map_len : 1, sizeof(bool));
    for (size_t i = 0; i < characters_len; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        ColorPair fp;
        memset(&fp, 0, sizeof(fp));
        if (ch->is_fill_character) {
            Color black;
            color_from_hex("#000000", &black);
            fp.has_fg = true;
            fp.fg = black;
        } else if (dynamic) {
            fp.has_fg = ch->animation.has_input_fg;
            fp.fg = ch->animation.input_fg_color;
            fp.has_bg = ch->animation.has_input_bg;
            fp.bg = ch->animation.input_bg_color;
        } else {
            const Color *mapped = coordcolormap_get(&mapping, ch->input_coord);
            fp.has_fg = true;
            if (mapped) {
                fp.fg = *mapped;
            }
        }
        if ((size_t)id < st->map_len) {
            st->final_colors[id] = fp;
            st->final_present[id] = true;
        }
    }
    free(characters);

    Gradient beam_gradient;
    memset(&beam_gradient, 0, sizeof(beam_gradient));
    if (gradient_new(cfg->beam_gradient_stops.items, cfg->beam_gradient_stops.len, cfg->beam_gradient_steps.items,
                     cfg->beam_gradient_steps.len, false, false, &beam_gradient) != 0) {
        coordcolormap_free(&mapping);
        gradient_free(&final_gradient);
        return -1;
    }

    BeamGroup *groups = NULL;
    size_t groups_len = 0;
    size_t groups_cap = 0;

    CharIdGrouping rows = terminal_get_characters_grouped(&ctx->terminal, all_chars, CG_ROW_TOP_TO_BOTTOM);
    for (size_t i = 0; i < rows.len; i++) {
        BeamsIdVec v;
        v.items = rows.buckets[i].items;
        v.len = rows.buckets[i].len;
        groupvec_push(&groups, &groups_len, &groups_cap, beams_make_group(ctx, cfg, v, DIR_ROW));
    }
    free(rows.buckets);

    CharIdGrouping columns = terminal_get_characters_grouped(&ctx->terminal, all_chars, CG_COLUMN_LEFT_TO_RIGHT);
    for (size_t i = 0; i < columns.len; i++) {
        BeamsIdVec v;
        v.items = columns.buckets[i].items;
        v.len = columns.buckets[i].len;
        groupvec_push(&groups, &groups_len, &groups_cap, beams_make_group(ctx, cfg, v, DIR_COLUMN));
    }
    free(columns.buckets);

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    for (size_t gi = 0; gi < groups_len && rc == 0; gi++) {
        BeamGroup *group = &groups[gi];
        if (group->direction != DIR_ROW) {
            continue;
        }
        for (size_t ci = 0; ci < group->characters.len && rc == 0; ci++) {
            CharId id = group->characters.items[ci];
            EffectCharacter *ch = &ctx->terminal.arena.items[id];
            char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
            strcpy(input_symbol, ch->input_symbol);
            bool uses_pre = ch->uses_input_preexisting_colors;

            animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "beam_row",
                                uses_pre);
            animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "beam_column",
                                uses_pre);
            animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "brighten",
                                uses_pre);
            ch = &ctx->terminal.arena.items[id];
            Scene *row_scene = (Scene *)om_get(&ch->animation.scenes, "beam_row");
            Scene *col_scene = (Scene *)om_get(&ch->animation.scenes, "beam_column");
            if (scene_apply_gradient_to_symbols(row_scene, (const char *const *)cfg->beam_row_symbols.items,
                                                cfg->beam_row_symbols.len, cfg->beam_gradient_frames,
                                                &beam_gradient, NULL) != 0) {
                rc = -1;
            }
            if (rc == 0 &&
                scene_apply_gradient_to_symbols(col_scene, (const char *const *)cfg->beam_column_symbols.items,
                                                cfg->beam_column_symbols.len, cfg->beam_gradient_frames,
                                                &beam_gradient, NULL) != 0) {
                rc = -1;
            }

            ColorPair char_colors;
            memset(&char_colors, 0, sizeof(char_colors));
            if ((size_t)id < st->map_len && st->final_present[id]) {
                char_colors = st->final_colors[id];
            }
            Gradient fg_fade;
            Gradient bg_fade;
            Gradient fg_bright;
            Gradient bg_bright;
            bool has_fg_fade = false;
            bool has_bg_fade = false;
            bool has_fg_bright = false;
            bool has_bg_bright = false;
            if (rc == 0 && char_colors.has_fg) {
                Color faded = color_adjust_brightness(&char_colors.fg, 0.3);
                if (gradient_pair(char_colors.fg, faded, 10, &fg_fade) != 0) {
                    rc = -1;
                } else {
                    has_fg_fade = true;
                }
                if (rc == 0) {
                    if (gradient_pair(faded, char_colors.fg, 10, &fg_bright) != 0) {
                        rc = -1;
                    } else {
                        has_fg_bright = true;
                    }
                }
            }
            if (rc == 0 && char_colors.has_bg) {
                Color faded = color_adjust_brightness(&char_colors.bg, 0.3);
                if (gradient_pair(char_colors.bg, faded, 10, &bg_fade) != 0) {
                    rc = -1;
                } else {
                    has_bg_fade = true;
                }
                if (rc == 0) {
                    if (gradient_pair(faded, char_colors.bg, 10, &bg_bright) != 0) {
                        rc = -1;
                    } else {
                        has_bg_bright = true;
                    }
                }
            }

            if (rc == 0) {
                ch = &ctx->terminal.arena.items[id];
                row_scene = (Scene *)om_get(&ch->animation.scenes, "beam_row");
                col_scene = (Scene *)om_get(&ch->animation.scenes, "beam_column");
                if (has_fg_fade || has_bg_fade) {
                    const char *syms[1] = {input_symbol};
                    if (scene_apply_gradient_to_symbols(row_scene, syms, 1, 2,
                                                        has_fg_fade ? &fg_fade : NULL,
                                                        has_bg_fade ? &bg_fade : NULL) != 0) {
                        rc = -1;
                    }
                    if (rc == 0) {
                        ch = &ctx->terminal.arena.items[id];
                        col_scene = (Scene *)om_get(&ch->animation.scenes, "beam_column");
                        if (scene_apply_gradient_to_symbols(col_scene, syms, 1, 2,
                                                            has_fg_fade ? &fg_fade : NULL,
                                                            has_bg_fade ? &bg_fade : NULL) != 0) {
                            rc = -1;
                        }
                    }
                } else {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    if (scene_add_frame(row_scene, input_symbol, 2, &vp) != 0) {
                        rc = -1;
                    }
                    if (rc == 0) {
                        ch = &ctx->terminal.arena.items[id];
                        col_scene = (Scene *)om_get(&ch->animation.scenes, "beam_column");
                        if (scene_add_frame(col_scene, input_symbol, 2, &vp) != 0) {
                            rc = -1;
                        }
                    }
                }
            }

            if (rc == 0) {
                ch = &ctx->terminal.arena.items[id];
                Scene *brighten_scene = (Scene *)om_get(&ch->animation.scenes, "brighten");
                if (has_fg_bright || has_bg_bright) {
                    const char *syms[1] = {input_symbol};
                    if (scene_apply_gradient_to_symbols(brighten_scene, syms, 1, cfg->final_gradient_frames,
                                                        has_fg_bright ? &fg_bright : NULL,
                                                        has_bg_bright ? &bg_bright : NULL) != 0) {
                        rc = -1;
                    }
                } else {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    if (scene_add_frame(brighten_scene, input_symbol, cfg->final_gradient_frames, &vp) != 0) {
                        rc = -1;
                    }
                }
            }

            if (has_fg_fade) {
                gradient_free(&fg_fade);
            }
            if (has_bg_fade) {
                gradient_free(&bg_fade);
            }
            if (has_fg_bright) {
                gradient_free(&fg_bright);
            }
            if (has_bg_bright) {
                gradient_free(&bg_bright);
            }
            free(input_symbol);
        }
    }

    coordcolormap_free(&mapping);
    gradient_free(&beam_gradient);
    gradient_free(&final_gradient);

    if (rc != 0) {
        groups_free(groups, groups_len);
        return -1;
    }

    st->pending_groups = groups;
    st->pending_len = groups_len;
    st->pending_cap = groups_cap;
    rng_shuffle(&ctx->rng, st->pending_groups, st->pending_len, sizeof(BeamGroup));
    return 0;
}

static char *beams_next_frame(Effect *self, EngineCtx *ctx) {
    Beams *st = self->state;
    BeamsConfig *cfg = &st->config;
    if (st->phase != BEAMS_COMPLETE || !ac_is_empty(&ctx->active_characters)) {
        switch (st->phase) {
            case BEAMS_BEAMS:
                if (st->delay == 0) {
                    if (st->pending_len > 0) {
                        int64_t n = rng_randint(&ctx->rng, 1, 5);
                        for (int64_t i = 0; i < n; i++) {
                            if (st->pending_len > 0) {
                                BeamGroup g = groupvec_remove0(st->pending_groups, &st->pending_len);
                                groupvec_push(&st->active_groups, &st->active_len, &st->active_cap, g);
                            }
                        }
                    }
                    st->delay = cfg->beam_delay;
                } else {
                    st->delay -= 1;
                }
                {
                    BeamGroup *active = st->active_groups;
                    size_t alen = st->active_len;
                    st->active_groups = NULL;
                    st->active_len = 0;
                    st->active_cap = 0;
                    for (size_t gi = 0; gi < alen; gi++) {
                        BeamGroup *group = &active[gi];
                        group->next_character_counter += group->speed;
                        int64_t count = (int64_t)group->next_character_counter;
                        if (count > 1) {
                            for (int64_t c = 0; c < count; c++) {
                                if (group->characters.len > 0) {
                                    CharId next;
                                    if (beams_get_next_character(ctx, self, group, &next)) {
                                        ac_insert(&ctx->active_characters, next);
                                    }
                                }
                            }
                        }
                    }
                    size_t keep = 0;
                    for (size_t gi = 0; gi < alen; gi++) {
                        if (active[gi].characters.len > 0) {
                            if (keep != gi) {
                                active[keep] = active[gi];
                            }
                            keep++;
                        } else {
                            group_free(&active[gi]);
                        }
                    }
                    st->active_groups = active;
                    st->active_len = keep;
                    st->active_cap = alen;
                }
                if (st->pending_len == 0 && st->active_len == 0 && ac_is_empty(&ctx->active_characters)) {
                    st->phase = BEAMS_FINAL_WIPE;
                }
                break;
            case BEAMS_FINAL_WIPE:
                if (st->final_wipe_len > 0) {
                    for (int64_t i = 0; i < cfg->final_wipe_speed; i++) {
                        if (st->final_wipe_len == 0) {
                            break;
                        }
                        BeamsIdVec next_group = st->final_wipe_groups[0];
                        for (size_t k = 1; k < st->final_wipe_len; k++) {
                            st->final_wipe_groups[k - 1] = st->final_wipe_groups[k];
                        }
                        st->final_wipe_len--;
                        for (size_t j = 0; j < next_group.len; j++) {
                            CharId id = next_group.items[j];
                            engine_activate_scene(ctx, self, id, "brighten");
                            terminal_set_character_visibility(&ctx->terminal, id, true);
                            ac_insert(&ctx->active_characters, id);
                        }
                        free(next_group.items);
                    }
                } else {
                    st->phase = BEAMS_COMPLETE;
                }
                break;
            case BEAMS_COMPLETE:
                break;
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void beams_destroy(Effect *self) {
    Beams *st = self->state;
    if (!st) {
        return;
    }
    groups_free(st->pending_groups, st->pending_len);
    groups_free(st->active_groups, st->active_len);
    for (size_t i = 0; i < st->final_wipe_len; i++) {
        free(st->final_wipe_groups[i].items);
    }
    free(st->final_wipe_groups);
    free(st->final_colors);
    free(st->final_present);
    free(st);
    free(self);
}

static const EffectOps BEAMS_OPS = {beams_build, beams_next_frame, beams_destroy, NULL};

Effect *beams_make(const void *cfg) {
    Beams *st = calloc(1, sizeof(Beams));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const BeamsConfig *)cfg;
    st->phase = BEAMS_BEAMS;
    effect->ops = &BEAMS_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec beams_specs[] = {
    {"beam-row-symbols", 0, EF_STRING_LIST, offsetof(BeamsConfig, beam_row_symbols), NULL},
    {"beam-column-symbols", 0, EF_STRING_LIST, offsetof(BeamsConfig, beam_column_symbols), NULL},
    EF_SPEC("beam-delay", 0, EF_POS_INT, offsetof(BeamsConfig, beam_delay)),
    EF_SPEC("beam-row-speed-range", 0, EF_INT_RANGE, offsetof(BeamsConfig, beam_row_speed_range)),
    EF_SPEC("beam-column-speed-range", 0, EF_INT_RANGE, offsetof(BeamsConfig, beam_column_speed_range)),
    EF_SPEC("beam-gradient-stops", 0, EF_COLOR_LIST, offsetof(BeamsConfig, beam_gradient_stops)),
    EF_SPEC("beam-gradient-steps", 0, EF_INT_LIST, offsetof(BeamsConfig, beam_gradient_steps)),
    EF_SPEC("beam-gradient-frames", 0, EF_POS_INT, offsetof(BeamsConfig, beam_gradient_frames)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(BeamsConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(BeamsConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-frames", 0, EF_POS_INT, offsetof(BeamsConfig, final_gradient_frames)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(BeamsConfig, final_gradient_direction)),
    EF_SPEC("final-wipe-speed", 0, EF_POS_INT, offsetof(BeamsConfig, final_wipe_speed)),
};

const EffectEntry beams_entry = {
    "beams",
    beams_specs,
    sizeof(beams_specs) / sizeof(beams_specs[0]),
    sizeof(BeamsConfig),
    beams_config_defaults,
    beams_free_config,
    beams_make,
};
