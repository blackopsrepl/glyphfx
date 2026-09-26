#include "effects/sweep.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/graphics.h"
#include "utils/sequence_easer.h"

typedef struct {
    SweepConfig config;
    Color *dynamic_palette;
    size_t dynamic_palette_len;
    bool complete;
    bool first_phase;
    SequenceEaser easer;
    bool has_easer;
    CharIdGrouping groups_first;
    CharIdGrouping groups_second;
} Sweep;

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

void sweep_config_defaults(void *cfg_ptr) {
    SweepConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    push_symbol_default(&cfg->sweep_symbols, "█");
    push_symbol_default(&cfg->sweep_symbols, "▓");
    push_symbol_default(&cfg->sweep_symbols, "▒");
    push_symbol_default(&cfg->sweep_symbols, "░");
    cfg->first_sweep_direction = CG_COLUMN_RIGHT_TO_LEFT;
    cfg->second_sweep_direction = CG_COLUMN_LEFT_TO_RIGHT;
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "ffffff");
    push_int_default(&cfg->final_gradient_steps, 8);
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void sweep_free_config(void *cfg_ptr) {
    SweepConfig *cfg = cfg_ptr;
    for (size_t i = 0; i < cfg->sweep_symbols.len; i++) {
        free(cfg->sweep_symbols.items[i]);
    }
    free(cfg->sweep_symbols.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int sweep_build(Effect *self, EngineCtx *ctx) {
    Sweep *st = self->state;
    SweepConfig *cfg = &st->config;

    Gradient final_fg_gradient;
    if (gradient_new(cfg->final_gradient_stops.items, cfg->final_gradient_stops.len,
                     cfg->final_gradient_steps.items, cfg->final_gradient_steps.len, false, false,
                     &final_fg_gradient) != 0) {
        return -1;
    }
    CoordColorMap mapping;
    if (gradient_build_coordinate_color_mapping(&final_fg_gradient, ctx->terminal.canvas.text_bottom,
                                                ctx->terminal.canvas.text_top, ctx->terminal.canvas.text_left,
                                                ctx->terminal.canvas.text_right,
                                                cfg->final_gradient_direction, &mapping) != 0) {
        gradient_free(&final_fg_gradient);
        return -1;
    }

    const char *shade_hex[5] = {"A0A0A0", "808080", "404040", "202020", "101010"};
    Color shades_of_gray[5];
    for (size_t i = 0; i < 5; i++) {
        color_from_hex(shade_hex[i], &shades_of_gray[i]);
    }

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    int rc = 0;

    if (dynamic) {
        size_t n = 0;
        CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                     CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);
        st->dynamic_palette = malloc((n ? n : 1) * 2 * sizeof(Color));
        for (size_t i = 0; i < n; i++) {
            EffectCharacter *ch = &ctx->terminal.arena.items[characters[i]];
            if (ch->animation.has_input_fg) {
                st->dynamic_palette[st->dynamic_palette_len++] = ch->animation.input_fg_color;
            }
            if (ch->animation.has_input_bg) {
                st->dynamic_palette[st->dynamic_palette_len++] = ch->animation.input_bg_color;
            }
        }
        free(characters);
        if (st->dynamic_palette_len == 0) {
            free(st->dynamic_palette);
            st->dynamic_palette = malloc(final_fg_gradient.len * sizeof(Color));
            memcpy(st->dynamic_palette, final_fg_gradient.spectrum, final_fg_gradient.len * sizeof(Color));
            st->dynamic_palette_len = final_fg_gradient.len;
        }
    }

    CharacterFilter fills_filter;
    fills_filter.input_chars = true;
    fills_filter.inner_fill_chars = true;
    fills_filter.outer_fill_chars = true;
    fills_filter.added_chars = false;

    size_t n = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, fills_filter,
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    for (size_t i = 0; i < n && rc == 0; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        bool is_fill = ch->is_fill_character;
        bool has_input_fg = ch->animation.has_input_fg;
        Color input_fg = ch->animation.input_fg_color;
        bool has_input_bg = ch->animation.has_input_bg;
        Color input_bg = ch->animation.input_bg_color;
        Coord input_coord = ch->input_coord;
        bool uses_pre = ch->uses_input_preexisting_colors;
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);

        ColorPair char_final;
        memset(&char_final, 0, sizeof(char_final));
        if (!is_fill) {
            if (dynamic) {
                char_final.has_fg = has_input_fg;
                char_final.fg = input_fg;
                char_final.has_bg = has_input_bg;
                char_final.bg = input_bg;
            } else {
                const Color *mapped = coordcolormap_get(&mapping, input_coord);
                char_final.has_fg = true;
                if (mapped) {
                    char_final.fg = *mapped;
                }
            }
        }

        animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "initial_sweep", uses_pre);
        Scene *scene = (Scene *)om_get(&ch->animation.scenes, "initial_sweep");
        for (size_t j = 0; j < cfg->sweep_symbols.len; j++) {
            Color color = shades_of_gray[rng_choice_index(&ctx->rng, 5)];
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = color;
            if (scene_add_frame(scene, cfg->sweep_symbols.items[j], 5, &vp) != 0) {
                rc = -1;
                break;
            }
        }
        if (rc == 0) {
            Color gray;
            color_from_hex("#808080", &gray);
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = gray;
            if (scene_add_frame(scene, input_symbol, 1, &vp) != 0) {
                rc = -1;
            }
        }

        if (rc == 0) {
            ch = &ctx->terminal.arena.items[id];
            animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "second_sweep", uses_pre);
            scene = (Scene *)om_get(&ch->animation.scenes, "second_sweep");
            for (size_t j = 0; j < cfg->sweep_symbols.len; j++) {
                Color color;
                if (dynamic) {
                    size_t k = rng_choice_index(&ctx->rng, st->dynamic_palette_len);
                    color = st->dynamic_palette[k];
                } else {
                    size_t k = rng_choice_index(&ctx->rng, final_fg_gradient.len);
                    color = final_fg_gradient.spectrum[k];
                }
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                vp.colors.has_fg = true;
                vp.colors.fg = color;
                if (scene_add_frame(scene, cfg->sweep_symbols.items[j], 5, &vp) != 0) {
                    rc = -1;
                    break;
                }
            }
            if (rc == 0) {
                ColorPair sweep_final;
                memset(&sweep_final, 0, sizeof(sweep_final));
                if (!is_fill) {
                    sweep_final = char_final;
                } else if (!dynamic) {
                    Color black;
                    color_from_hex("000000", &black);
                    sweep_final.has_fg = true;
                    sweep_final.fg = black;
                }
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                vp.colors = sweep_final;
                if (scene_add_frame(scene, input_symbol, 1, &vp) != 0) {
                    rc = -1;
                }
            }
        }
        free(input_symbol);
    }
    free(characters);

    if (rc == 0) {
        st->groups_first =
            terminal_get_characters_grouped(&ctx->terminal, fills_filter, cfg->first_sweep_direction);
        sequence_easer_init(&st->easer, st->groups_first.buckets, st->groups_first.len,
                            easing_named(EASE_IN_OUT_CIRC), 100);
        st->has_easer = true;
        st->groups_second =
            terminal_get_characters_grouped(&ctx->terminal, fills_filter, cfg->second_sweep_direction);
        st->first_phase = true;
    }

    coordcolormap_free(&mapping);
    gradient_free(&final_fg_gradient);
    return rc;
}

static const char *sweep_next_frame(Effect *self, EngineCtx *ctx) {
    Sweep *st = self->state;
    if (!ac_is_empty(&ctx->active_characters) || !st->complete) {
        SequenceStep step = sequence_easer_step(&st->easer);
        for (size_t g = 0; g < step.added_len; g++) {
            for (size_t k = 0; k < step.added[g].len; k++) {
                CharId id = step.added[g].items[k];
                if (st->first_phase) {
                    terminal_set_character_visibility(&ctx->terminal, id, true);
                }
                engine_activate_scene(ctx, self, id, st->first_phase ? "initial_sweep" : "second_sweep");
            }
            for (size_t k = 0; k < step.added[g].len; k++) {
                ac_insert(&ctx->active_characters, step.added[g].items[k]);
            }
        }
        bool easer_complete = sequence_easer_is_complete(&st->easer);
        if (easer_complete && st->first_phase) {
            st->easer.sequence = st->groups_second.buckets;
            st->easer.len = st->groups_second.len;
            sequence_easer_reset(&st->easer);
            st->first_phase = false;
        } else if (easer_complete && !st->first_phase) {
            st->complete = true;
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void sweep_destroy(Effect *self) {
    Sweep *st = self->state;
    if (!st) {
        return;
    }
    charidgrouping_free(&st->groups_first);
    charidgrouping_free(&st->groups_second);
    free(st->dynamic_palette);
    free(st);
    free(self);
}

static const EffectOps SWEEP_OPS = {sweep_build, sweep_next_frame, sweep_destroy, NULL};

Effect *sweep_make(const void *cfg) {
    Sweep *st = calloc(1, sizeof(Sweep));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const SweepConfig *)cfg;
    effect->ops = &SWEEP_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec sweep_specs[] = {
    {"sweep-symbols", 0, EF_STRING_LIST, offsetof(SweepConfig, sweep_symbols), NULL},
    EF_SPEC("first-sweep-direction", 0, EF_CHAR_GROUP, offsetof(SweepConfig, first_sweep_direction)),
    EF_SPEC("second-sweep-direction", 0, EF_CHAR_GROUP, offsetof(SweepConfig, second_sweep_direction)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(SweepConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(SweepConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(SweepConfig, final_gradient_direction)),
};

const EffectEntry sweep_entry = {
    "sweep",
    sweep_specs,
    sizeof(sweep_specs) / sizeof(sweep_specs[0]),
    sizeof(SweepConfig),
    sweep_config_defaults,
    sweep_free_config,
    sweep_make,
};
