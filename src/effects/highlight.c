#include "effects/highlight.h"

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
    HighlightConfig config;
    CharIdGrouping groups;
    SequenceEaser easer;
    bool has_easer;
    // The reference stores character_final_color_map but never reads it; the
    // dense mirror is kept so the build walk matches, though it is dead state.
    Color *base_color_map;
    bool *base_color_present;
    size_t base_color_len;
} Highlight;

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

void highlight_config_defaults(void *cfg_ptr) {
    HighlightConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->highlight_brightness = 1.75;
    cfg->highlight_direction = CG_DIAGONAL_BOTTOM_LEFT_TO_TOP_RIGHT;
    cfg->highlight_width = 8;
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "FFFFFF");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void highlight_free_config(void *cfg_ptr) {
    HighlightConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int highlight_build(Effect *self, EngineCtx *ctx) {
    Highlight *st = self->state;
    HighlightConfig *cfg = &st->config;

    st->groups = terminal_get_characters_grouped(&ctx->terminal, character_filter_default(),
                                                 cfg->highlight_direction);
    sequence_easer_init(&st->easer, st->groups.buckets, st->groups.len, easing_named(EASE_IN_OUT_CIRC), 100);
    st->has_easer = true;

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

    size_t arena_len = ctx->terminal.arena.len;
    st->base_color_len = arena_len;
    st->base_color_map = calloc(arena_len ? arena_len : 1, sizeof(Color));
    st->base_color_present = calloc(arena_len ? arena_len : 1, sizeof(bool));

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        bool has_input_fg = ch->animation.has_input_fg;
        Color input_fg = ch->animation.input_fg_color;
        bool has_input_bg = ch->animation.has_input_bg;
        Color input_bg = ch->animation.input_bg_color;
        Coord input_coord = ch->input_coord;
        bool uses_pre = ch->uses_input_preexisting_colors;
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);

        bool base_present;
        Color base_color;
        bool bg_present;
        Color bg_color;
        if (dynamic) {
            base_present = has_input_fg;
            base_color = input_fg;
            bg_present = has_input_bg;
            bg_color = input_bg;
        } else {
            const Color *mapped = coordcolormap_get(&mapping, input_coord);
            base_present = true;
            memset(&base_color, 0, sizeof(base_color));
            if (mapped) {
                base_color = *mapped;
            }
            bg_present = false;
            memset(&bg_color, 0, sizeof(bg_color));
        }
        if ((size_t)id < st->base_color_len) {
            st->base_color_present[id] = base_present;
            st->base_color_map[id] = base_color;
        }

        ColorPair base_colors;
        memset(&base_colors, 0, sizeof(base_colors));
        base_colors.has_fg = base_present;
        base_colors.fg = base_color;
        base_colors.has_bg = bg_present;
        base_colors.bg = bg_color;

        Gradient highlight_gradient;
        bool has_highlight_gradient = false;
        if (base_present) {
            Color highlight_color = color_adjust_brightness(&base_color, cfg->highlight_brightness);
            Color stops[4] = {base_color, highlight_color, highlight_color, base_color};
            int64_t steps[3] = {3, cfg->highlight_width, 3};
            if (gradient_new(stops, 4, steps, 3, false, false, &highlight_gradient) == 0) {
                has_highlight_gradient = true;
            } else {
                rc = -1;
            }
        }

        if (rc == 0) {
            ch = &ctx->terminal.arena.items[id];
            animation_set_appearance(&ch->animation, uses_pre, input_symbol, &base_colors);
            animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "highlight", uses_pre);
            Scene *scene = (Scene *)om_get(&ch->animation.scenes, "highlight");
            if (has_highlight_gradient) {
                for (size_t j = 0; j < highlight_gradient.len; j++) {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    vp.colors.has_fg = true;
                    vp.colors.fg = highlight_gradient.spectrum[j];
                    vp.colors.has_bg = bg_present;
                    vp.colors.bg = bg_color;
                    if (scene_add_frame(scene, input_symbol, 2, &vp) != 0) {
                        rc = -1;
                        break;
                    }
                }
            } else {
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                vp.colors = base_colors;
                if (scene_add_frame(scene, input_symbol, 2, &vp) != 0) {
                    rc = -1;
                }
            }
            if (has_highlight_gradient) {
                gradient_free(&highlight_gradient);
            }
        }
        if (rc == 0) {
            terminal_set_character_visibility(&ctx->terminal, id, true);
        }
        free(input_symbol);
    }

    free(characters);
    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    return rc;
}

static char *highlight_next_frame(Effect *self, EngineCtx *ctx) {
    Highlight *st = self->state;
    bool easer_complete = sequence_easer_is_complete(&st->easer);
    if (!ac_is_empty(&ctx->active_characters) || !easer_complete) {
        SequenceStep step = sequence_easer_step(&st->easer);
        for (size_t g = 0; g < step.added_len; g++) {
            for (size_t k = 0; k < step.added[g].len; k++) {
                CharId id = step.added[g].items[k];
                engine_activate_scene(ctx, self, id, "highlight");
                ac_insert(&ctx->active_characters, id);
            }
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void highlight_destroy(Effect *self) {
    Highlight *st = self->state;
    if (!st) {
        return;
    }
    charidgrouping_free(&st->groups);
    free(st->base_color_map);
    free(st->base_color_present);
    free(st);
    free(self);
}

static const EffectOps HIGHLIGHT_OPS = {highlight_build, highlight_next_frame, highlight_destroy, NULL};

Effect *highlight_make(const void *cfg) {
    Highlight *st = calloc(1, sizeof(Highlight));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const HighlightConfig *)cfg;
    effect->ops = &HIGHLIGHT_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec highlight_specs[] = {
    EF_SPEC("highlight-brightness", 0, EF_FLOAT_POS, offsetof(HighlightConfig, highlight_brightness)),
    EF_SPEC("highlight-direction", 0, EF_CHAR_GROUP, offsetof(HighlightConfig, highlight_direction)),
    EF_SPEC("highlight-width", 0, EF_POS_INT, offsetof(HighlightConfig, highlight_width)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(HighlightConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(HighlightConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(HighlightConfig, final_gradient_direction)),
};

const EffectEntry highlight_entry = {
    "highlight",
    highlight_specs,
    sizeof(highlight_specs) / sizeof(highlight_specs[0]),
    sizeof(HighlightConfig),
    highlight_config_defaults,
    highlight_free_config,
    highlight_make,
};
