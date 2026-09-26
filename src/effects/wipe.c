#include "effects/wipe.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/sequence_easer.h"

typedef struct {
    WipeConfig config;
    CharIdGrouping groups;
    SequenceEaser easer;
    bool has_easer;
    int64_t wipe_delay;
} Wipe;

static void push_color_default(ColorList *list, const char *hex) {
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 4;
        list->items = realloc(list->items, cap * sizeof(Color));
        list->cap = cap;
    }
    Color c;
    color_from_hex(hex, &c);
    list->items[list->len++] = c;
}

static void push_int_default(IntList *list, int64_t v) {
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 4;
        list->items = realloc(list->items, cap * sizeof(int64_t));
        list->cap = cap;
    }
    list->items[list->len++] = v;
}

void wipe_config_defaults(void *cfg_ptr) {
    WipeConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->wipe_direction = CG_DIAGONAL_TOP_LEFT_TO_BOTTOM_RIGHT;
    cfg->wipe_delay = 0;
    easing_parse("in_out_circ", &cfg->wipe_ease);
    push_color_default(&cfg->final_gradient_stops, "833ab4");
    push_color_default(&cfg->final_gradient_stops, "fd1d1d");
    push_color_default(&cfg->final_gradient_stops, "fcb045");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_frames = 3;
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void wipe_free_config(void *cfg_ptr) {
    WipeConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int wipe_build(Effect *self, EngineCtx *ctx) {
    Wipe *st = self->state;
    WipeConfig *cfg = &st->config;

    st->groups = terminal_get_characters_grouped(&ctx->terminal, character_filter_default(), cfg->wipe_direction);
    sequence_easer_init(&st->easer, st->groups.buckets, st->groups.len, cfg->wipe_ease, 100);
    st->has_easer = true;
    st->wipe_delay = cfg->wipe_delay;

    Gradient final_gradient;
    if (gradient_new(cfg->final_gradient_stops.items, cfg->final_gradient_stops.len, cfg->final_gradient_steps.items,
                     cfg->final_gradient_steps.len, false, false, &final_gradient) != 0) {
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
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n_chars);
    int64_t gradient_steps_sum = 0;
    for (size_t i = 0; i < cfg->final_gradient_steps.len; i++) {
        gradient_steps_sum += cfg->final_gradient_steps.items[i];
    }
    int rc = 0;
    for (size_t i = 0; i < n_chars && rc == 0; i++) {
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

        Easing no_ease;
        memset(&no_ease, 0, sizeof(no_ease));
        const char *scene_id =
            animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "wipe", uses_pre);
        Scene *scene = (Scene *)om_get(&ch->animation.scenes, scene_id);
        const char *symbols[1] = {input_symbol};
        if (dynamic) {
            int64_t frame_count = gradient_steps_sum + 1;
            for (int64_t f = 0; f < frame_count && rc == 0; f++) {
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                vp.colors = final_colors;
                if (scene_add_frame(scene, input_symbol, cfg->final_gradient_frames, &vp) != 0) {
                    rc = -1;
                }
            }
        } else {
            Color stops[2] = {final_gradient.spectrum[0], final_colors.fg};
            Gradient wipe_gradient;
            if (gradient_new(stops, 2, cfg->final_gradient_steps.items, cfg->final_gradient_steps.len, false, false,
                             &wipe_gradient) != 0) {
                rc = -1;
            } else {
                if (scene_apply_gradient_to_symbols(scene, symbols, 1, cfg->final_gradient_frames, &wipe_gradient,
                                                    NULL) != 0) {
                    rc = -1;
                }
                gradient_free(&wipe_gradient);
            }
        }
        free(input_symbol);
    }
    free(characters);
    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    return rc;
}

static char *wipe_next_frame(Effect *self, EngineCtx *ctx) {
    Wipe *st = self->state;
    bool easer_complete = sequence_easer_is_complete(&st->easer);
    if (ac_is_empty(&ctx->active_characters) && easer_complete) {
        return NULL;
    }
    if (st->wipe_delay == 0) {
        SequenceStep step = sequence_easer_step(&st->easer);
        for (size_t g = 0; g < step.added_len; g++) {
            for (size_t k = 0; k < step.added[g].len; k++) {
                CharId id = step.added[g].items[k];
                engine_activate_scene(ctx, self, id, "wipe");
                terminal_set_character_visibility(&ctx->terminal, id, true);
                ac_insert(&ctx->active_characters, id);
            }
        }
        for (size_t g = 0; g < step.removed_len; g++) {
            for (size_t k = 0; k < step.removed[g].len; k++) {
                CharId id = step.removed[g].items[k];
                engine_deactivate_scene(ctx, id, NULL);
                Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "wipe");
                if (scene) {
                    scene_reset(scene);
                }
                terminal_set_character_visibility(&ctx->terminal, id, false);
            }
        }
        st->wipe_delay = st->config.wipe_delay;
    } else {
        st->wipe_delay -= 1;
    }
    engine_update(ctx, self);
    return engine_frame(ctx);
}

static void wipe_destroy(Effect *self) {
    Wipe *st = self->state;
    if (!st) {
        return;
    }
    charidgrouping_free(&st->groups);
    free(st);
    free(self);
}

static const EffectOps WIPE_OPS = {wipe_build, wipe_next_frame, wipe_destroy, NULL};

Effect *wipe_make(const void *cfg) {
    Wipe *st = calloc(1, sizeof(Wipe));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const WipeConfig *)cfg;
    effect->ops = &WIPE_OPS;
    effect->state = st;
    return effect;
}

const EffectEntry wipe_entry = {
    "wipe",
    (const EffOptSpec[]){
        EF_SPEC("wipe-direction", 0, EF_CHAR_GROUP, offsetof(WipeConfig, wipe_direction)),
        EF_SPEC("wipe-delay", 0, EF_NONNEG_INT, offsetof(WipeConfig, wipe_delay)),
        EF_SPEC("wipe-ease", 0, EF_EASING, offsetof(WipeConfig, wipe_ease)),
        EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(WipeConfig, final_gradient_stops)),
        EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(WipeConfig, final_gradient_steps)),
        EF_SPEC("final-gradient-frames", 0, EF_INT, offsetof(WipeConfig, final_gradient_frames)),
        EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(WipeConfig, final_gradient_direction)),
    },
    7,
    sizeof(WipeConfig),
    wipe_config_defaults,
    wipe_free_config,
    wipe_make,
};
