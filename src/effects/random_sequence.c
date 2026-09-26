#include "effects/random_sequence.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"

#define DYNAMIC_NEUTRAL_GRAY "808080"

void randomsequence_free_config(void *cfg_ptr) {
    RandomSequenceConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
    cfg->final_gradient_stops.items = NULL;
    cfg->final_gradient_steps.items = NULL;
}

static void push_color_default(ColorList *list, const char *hex);
static void push_int_default(IntList *list, int64_t v);

const size_t randomsequence_specs_len = 5;

const EffOptSpec randomsequence_specs[] = {
    EF_SPEC("speed", 0, EF_FLOAT_POS, offsetof(RandomSequenceConfig, speed)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(RandomSequenceConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(RandomSequenceConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-frames", 0, EF_INT, offsetof(RandomSequenceConfig, final_gradient_frames)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION,
            offsetof(RandomSequenceConfig, final_gradient_direction)),
};

void randomsequence_config_defaults(void *cfg_ptr) {
    RandomSequenceConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->speed = 0.007;
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "FFFFFF");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_frames = 8;
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

typedef struct {
    RandomSequenceConfig config;
    CharId *pending;
    size_t pending_len;
    size_t pending_cap;
    int64_t characters_per_tick;
} RandomSequenceState;

static void pending_push(RandomSequenceState *st, CharId id) {
    if (st->pending_len == st->pending_cap) {
        size_t cap = st->pending_cap ? st->pending_cap * 2 : 64;
        CharId *grown = realloc(st->pending, cap * sizeof(CharId));
        if (!grown) {
            return;
        }
        st->pending = grown;
        st->pending_cap = cap;
    }
    st->pending[st->pending_len++] = id;
}

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

static int rs_build(Effect *self, EngineCtx *ctx) {
    RandomSequenceState *st = self->state;
    RandomSequenceConfig *cfg = &st->config;

    st->characters_per_tick =
        (int64_t)(cfg->speed * (double)ctx->terminal.input_characters_len);
    if (st->characters_per_tick < 1) {
        st->characters_per_tick = 1;
    }

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
    size_t n_chars = 0;
    CharId *characters =
        terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n_chars);

    int rc = 0;
    for (size_t i = 0; i < n_chars; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        ColorPair final_colors;
        memset(&final_colors, 0, sizeof(final_colors));
        if (dynamic) {
            final_colors.has_fg = ch->animation.has_input_fg;
            final_colors.fg = ch->animation.input_fg_color;
            final_colors.has_bg = ch->animation.has_input_bg;
            final_colors.bg = ch->animation.input_bg_color;
        } else {
            const Color *c = coordcolormap_get(&mapping, ch->input_coord);
            final_colors.has_fg = true;
            if (c) {
                final_colors.fg = *c;
            }
        }
        bool uses_pre = ch->uses_input_preexisting_colors;
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);

        terminal_set_character_visibility(&ctx->terminal, id, false);
        Easing no_ease;
        memset(&no_ease, 0, sizeof(no_ease));
        const char *scene_id =
            animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "", uses_pre);
        Scene *scene = (Scene *)om_get(&ch->animation.scenes, scene_id);
        const char *symbols[1] = {input_symbol};
        int64_t frames = cfg->final_gradient_frames;
        if (dynamic) {
            if (final_colors.has_fg || final_colors.has_bg) {
                Gradient fg_grad;
                Gradient bg_grad;
                Gradient *fgp = NULL;
                Gradient *bgp = NULL;
                if (final_colors.has_fg) {
                    Color stops[2] = {ctx->terminal.config.terminal_background_color, final_colors.fg};
                    if (gradient_with_steps(stops, 2, 7, false, &fg_grad) != 0) {
                        rc = -1;
                    } else {
                        fgp = &fg_grad;
                    }
                }
                if (final_colors.has_bg) {
                    Color stops[2] = {ctx->terminal.config.terminal_background_color, final_colors.bg};
                    if (gradient_with_steps(stops, 2, 7, false, &bg_grad) != 0) {
                        rc = -1;
                    } else {
                        bgp = &bg_grad;
                    }
                }
                if (rc == 0 && scene_apply_gradient_to_symbols(scene, symbols, 1, frames, fgp, bgp) != 0) {
                    rc = -1;
                }
                if (fgp) gradient_free(&fg_grad);
                if (bgp) gradient_free(&bg_grad);
            } else {
                Color neutral;
                color_from_hex(DYNAMIC_NEUTRAL_GRAY, &neutral);
                Color stops[2] = {ctx->terminal.config.terminal_background_color, neutral};
                Gradient neutral_grad;
                if (gradient_with_steps(stops, 2, 7, false, &neutral_grad) != 0) {
                    rc = -1;
                } else {
                    if (scene_apply_gradient_to_symbols(scene, symbols, 1, frames, &neutral_grad, NULL) != 0) {
                        rc = -1;
                    }
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    if (rc == 0 && scene_add_frame(scene, input_symbol, frames, &vp) != 0) {
                        rc = -1;
                    }
                    gradient_free(&neutral_grad);
                }
            }
        } else {
            Color stops[2] = {ctx->terminal.config.terminal_background_color, final_colors.fg};
            Gradient gradient;
            if (gradient_with_steps(stops, 2, 7, false, &gradient) != 0) {
                rc = -1;
            } else {
                if (scene_apply_gradient_to_symbols(scene, symbols, 1, frames, &gradient, NULL) != 0) {
                    rc = -1;
                }
                gradient_free(&gradient);
            }
        }

        engine_activate_scene(ctx, self, id, scene_id);
        pending_push(st, id);
        free(input_symbol);
        if (rc != 0) {
            break;
        }
    }
    free(characters);
    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    if (rc != 0) {
        return -1;
    }
    rng_shuffle(&ctx->rng, st->pending, st->pending_len, sizeof(CharId));
    return 0;
}

static char *rs_next_frame(Effect *self, EngineCtx *ctx) {
    RandomSequenceState *st = self->state;
    if (st->pending_len == 0 && ac_is_empty(&ctx->active_characters)) {
        return NULL;
    }
    for (int64_t i = 0; i < st->characters_per_tick; i++) {
        if (st->pending_len == 0) {
            break;
        }
        CharId id = st->pending[--st->pending_len];
        terminal_set_character_visibility(&ctx->terminal, id, true);
        ac_insert(&ctx->active_characters, id);
    }
    engine_update(ctx, self);
    return engine_frame(ctx);
}

static void rs_destroy(Effect *self) {
    RandomSequenceState *st = self->state;
    if (!st) {
        return;
    }
    free(st->pending);
    free(st);
    free(self);
}

static const EffectOps RS_OPS = {
    rs_build,
    rs_next_frame,
    rs_destroy,
    NULL,
};

Effect *randomsequence_make(const void *cfg) {
    RandomSequenceState *st = calloc(1, sizeof(RandomSequenceState));
    if (!st) {
        return NULL;
    }
    st->config = *(const RandomSequenceConfig *)cfg;
    st->characters_per_tick = 1;
    Effect *effect = calloc(1, sizeof(Effect));
    if (!effect) {
        free(st);
        return NULL;
    }
    effect->ops = &RS_OPS;
    effect->state = st;
    return effect;
}

const EffectEntry randomsequence_entry = {
    "randomsequence",
    randomsequence_specs,
    sizeof(randomsequence_specs) / sizeof(randomsequence_specs[0]),
    sizeof(RandomSequenceConfig),
    randomsequence_config_defaults,
    randomsequence_free_config,
    randomsequence_make,
};
