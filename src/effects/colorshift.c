#include "effects/colorshift.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/geometry.h"
#include "utils/graphics.h"

typedef struct {
    ColorShiftConfig config;
    // Dense CharId -> final gradient color (reference HashMap; key reads only).
    Color *final_colors;
    bool *final_colors_present;
    size_t final_colors_len;
    // Dense CharId -> loop count (reference HashMap; key reads only).
    int64_t *loop_tracker;
    bool *loop_tracker_present;
    size_t loop_tracker_len;
} ColorShift;

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

void colorshift_config_defaults(void *cfg_ptr) {
    ColorShiftConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    const char *stops[7] = {"e81416", "ffa500", "faeb36", "79c314", "487de7", "4b369d", "70369d"};
    for (size_t i = 0; i < 7; i++) {
        push_color_default(&cfg->gradient_stops, stops[i]);
    }
    push_int_default(&cfg->gradient_steps, 12);
    cfg->gradient_frames = 2;
    cfg->no_travel = false;
    cfg->travel_direction = GRADIENT_RADIAL;
    cfg->reverse_travel_direction = false;
    cfg->no_loop = false;
    cfg->cycles = 3;
    cfg->skip_final_gradient = false;
    for (size_t i = 0; i < 7; i++) {
        push_color_default(&cfg->final_gradient_stops, stops[i]);
    }
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void colorshift_free_config(void *cfg_ptr) {
    ColorShiftConfig *cfg = cfg_ptr;
    free(cfg->gradient_stops.items);
    free(cfg->gradient_steps.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

// ColorShiftIterator.loop_tracker.
static void colorshift_callback(Effect *self, EngineCtx *ctx, CharId character, const EffectCallback *cb) {
    (void)cb;
    ColorShift *st = self->state;
    int64_t count = 0;
    if ((size_t)character < st->loop_tracker_len) {
        if (!st->loop_tracker_present[character]) {
            st->loop_tracker_present[character] = true;
            st->loop_tracker[character] = 0;
        }
        st->loop_tracker[character] += 1;
        count = st->loop_tracker[character];
    }
    if (st->config.cycles == 0 || count < st->config.cycles) {
        engine_activate_scene(ctx, self, character, "gradient");
    } else if (!st->config.skip_final_gradient) {
        engine_activate_scene(ctx, self, character, "final_gradient");
    }
}

static int colorshift_build(Effect *self, EngineCtx *ctx) {
    ColorShift *st = self->state;
    ColorShiftConfig *cfg = &st->config;

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

    size_t n = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);

    size_t arena_len = ctx->terminal.arena.len;
    st->final_colors_len = arena_len;
    st->final_colors = calloc(arena_len ? arena_len : 1, sizeof(Color));
    st->final_colors_present = calloc(arena_len ? arena_len : 1, sizeof(bool));
    for (size_t i = 0; i < n; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        const Color *mapped = coordcolormap_get(&mapping, ch->input_coord);
        if ((size_t)id < st->final_colors_len) {
            st->final_colors_present[id] = true;
            if (mapped) {
                st->final_colors[id] = *mapped;
            }
        }
    }

    Gradient gradient;
    if (gradient_new(cfg->gradient_stops.items, cfg->gradient_stops.len, cfg->gradient_steps.items,
                     cfg->gradient_steps.len, false, !cfg->no_loop, &gradient) != 0) {
        free(characters);
        coordcolormap_free(&mapping);
        gradient_free(&final_gradient);
        return -1;
    }

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    free(characters);
    characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                         CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);

    st->loop_tracker_len = arena_len;
    st->loop_tracker = calloc(arena_len ? arena_len : 1, sizeof(int64_t));
    st->loop_tracker_present = calloc(arena_len ? arena_len : 1, sizeof(bool));

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        CharId id = characters[i];
        terminal_set_character_visibility(&ctx->terminal, id, true);
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        bool has_input_fg = ch->animation.has_input_fg;
        Color input_fg = ch->animation.input_fg_color;
        bool has_input_bg = ch->animation.has_input_bg;
        Color input_bg = ch->animation.input_bg_color;
        Coord input_coord = ch->input_coord;
        bool uses_pre = ch->uses_input_preexisting_colors;
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);

        Color *colors = NULL;
        size_t colors_len = 0;
        bool colors_owned = false;
        if (cfg->no_travel) {
            colors = gradient.spectrum;
            colors_len = gradient.len;
        } else {
            double direction_index;
            bool direction_ok = true;
            if (cfg->travel_direction == GRADIENT_HORIZONTAL) {
                direction_index = (double)input_coord.column / (double)ctx->terminal.canvas.right;
            } else if (cfg->travel_direction == GRADIENT_VERTICAL) {
                direction_index = (double)input_coord.row / (double)ctx->terminal.canvas.top;
            } else if (cfg->travel_direction == GRADIENT_DIAGONAL) {
                direction_index = (double)(input_coord.row + input_coord.column) /
                                  (double)(ctx->terminal.canvas.right + ctx->terminal.canvas.top);
            } else {
                if (find_normalized_distance_from_center(
                        ctx->terminal.canvas.text_bottom, ctx->terminal.canvas.text_top,
                        ctx->terminal.canvas.text_left, ctx->terminal.canvas.text_right, input_coord,
                        &direction_index) != 0) {
                    direction_ok = false;
                }
            }
            if (!direction_ok) {
                free(input_symbol);
                rc = -1;
                break;
            }
            int64_t shift_distance = (int64_t)((double)gradient.len * direction_index);
            if (cfg->reverse_travel_direction) {
                shift_distance *= -1;
            }
            int64_t len = (int64_t)gradient.len;
            size_t k;
            if (shift_distance < 0) {
                int64_t wrapped = len + shift_distance;
                if (wrapped < 0) {
                    wrapped = 0;
                }
                k = (size_t)wrapped;
            } else {
                k = shift_distance < len ? (size_t)shift_distance : (size_t)len;
            }
            colors_len = gradient.len;
            colors = malloc((colors_len ? colors_len : 1) * sizeof(Color));
            for (size_t j = 0; j < colors_len; j++) {
                colors[j] = gradient.spectrum[(k + j) % colors_len];
            }
            colors_owned = true;
        }

        ch = &ctx->terminal.arena.items[id];
        animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "gradient", uses_pre);
        Scene *gradient_scene = (Scene *)om_get(&ch->animation.scenes, "gradient");
        for (size_t j = 0; j < colors_len; j++) {
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = colors[j];
            if (scene_add_frame(gradient_scene, input_symbol, cfg->gradient_frames, &vp) != 0) {
                rc = -1;
                break;
            }
        }
        if (rc != 0) {
            if (colors_owned) {
                free(colors);
            }
            free(input_symbol);
            break;
        }

        ch = &ctx->terminal.arena.items[id];
        animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "final_gradient", uses_pre);
        Color last_color = colors[colors_len - 1];

        if (dynamic) {
            Gradient fg_grad;
            Gradient bg_grad;
            bool has_fg_grad = false;
            bool has_bg_grad = false;
            if (has_input_fg) {
                Color stops[2] = {last_color, input_fg};
                if (gradient_with_steps(stops, 2, 8, false, &fg_grad) == 0) {
                    has_fg_grad = true;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0 && has_input_bg) {
                Color stops[2] = {last_color, input_bg};
                if (gradient_with_steps(stops, 2, 8, false, &bg_grad) == 0) {
                    has_bg_grad = true;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0) {
                Scene *scene =
                    (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "final_gradient");
                if (has_fg_grad || has_bg_grad) {
                    const char *symbols[1] = {input_symbol};
                    if (scene_apply_gradient_to_symbols(scene, symbols, 1, cfg->gradient_frames,
                                                        has_fg_grad ? &fg_grad : NULL,
                                                        has_bg_grad ? &bg_grad : NULL) != 0) {
                        rc = -1;
                    }
                } else {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    if (scene_add_frame(scene, input_symbol, cfg->gradient_frames, &vp) != 0) {
                        rc = -1;
                    }
                }
            }
            if (has_fg_grad) {
                gradient_free(&fg_grad);
            }
            if (has_bg_grad) {
                gradient_free(&bg_grad);
            }
        } else {
            Color final_color = st->final_colors[id];
            Color stops[2] = {last_color, final_color};
            Gradient final_scene_gradient;
            if (gradient_with_steps(stops, 2, 8, false, &final_scene_gradient) != 0) {
                rc = -1;
            } else {
                Scene *scene =
                    (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "final_gradient");
                for (size_t j = 0; j < final_scene_gradient.len; j++) {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    vp.colors.has_fg = true;
                    vp.colors.fg = final_scene_gradient.spectrum[j];
                    if (scene_add_frame(scene, input_symbol, cfg->gradient_frames, &vp) != 0) {
                        rc = -1;
                        break;
                    }
                }
                gradient_free(&final_scene_gradient);
            }
        }
        if (colors_owned) {
            free(colors);
        }
        if (rc != 0) {
            free(input_symbol);
            break;
        }

        engine_activate_scene(ctx, self, id, "gradient");
        ac_insert(&ctx->active_characters, id);

        CallerKey caller;
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_SCENE;
        caller.id = "gradient";
        EventAction callback_action;
        memset(&callback_action, 0, sizeof(callback_action));
        callback_action.kind = ACTION_CALLBACK;
        callback_action.cb.id = 0;
        if (engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &callback_action) != 0) {
            free(input_symbol);
            rc = -1;
            break;
        }
        free(input_symbol);
    }

    free(characters);
    coordcolormap_free(&mapping);
    gradient_free(&gradient);
    gradient_free(&final_gradient);
    return rc;
}

static const char *colorshift_next_frame(Effect *self, EngineCtx *ctx) {
    if (!ac_is_empty(&ctx->active_characters)) {
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void colorshift_destroy(Effect *self) {
    ColorShift *st = self->state;
    if (!st) {
        return;
    }
    free(st->final_colors);
    free(st->final_colors_present);
    free(st->loop_tracker);
    free(st->loop_tracker_present);
    free(st);
    free(self);
}

static const EffectOps COLORSHIFT_OPS = {colorshift_build, colorshift_next_frame, colorshift_destroy,
                                         colorshift_callback};

Effect *colorshift_make(const void *cfg) {
    ColorShift *st = calloc(1, sizeof(ColorShift));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const ColorShiftConfig *)cfg;
    effect->ops = &COLORSHIFT_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec colorshift_specs[] = {
    EF_SPEC("gradient-stops", 0, EF_COLOR_LIST, offsetof(ColorShiftConfig, gradient_stops)),
    EF_SPEC("gradient-steps", 0, EF_INT_LIST, offsetof(ColorShiftConfig, gradient_steps)),
    EF_SPEC("gradient-frames", 0, EF_POS_INT, offsetof(ColorShiftConfig, gradient_frames)),
    EF_SPEC("no-travel", 0, EF_FLAG, offsetof(ColorShiftConfig, no_travel)),
    EF_SPEC("travel-direction", 0, EF_DIRECTION, offsetof(ColorShiftConfig, travel_direction)),
    EF_SPEC("reverse-travel-direction", 0, EF_FLAG, offsetof(ColorShiftConfig, reverse_travel_direction)),
    EF_SPEC("no-loop", 0, EF_FLAG, offsetof(ColorShiftConfig, no_loop)),
    EF_SPEC("cycles", 0, EF_POS_INT, offsetof(ColorShiftConfig, cycles)),
    EF_SPEC("skip-final-gradient", 0, EF_FLAG, offsetof(ColorShiftConfig, skip_final_gradient)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(ColorShiftConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(ColorShiftConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(ColorShiftConfig, final_gradient_direction)),
};

const EffectEntry colorshift_entry = {
    "colorshift",
    colorshift_specs,
    sizeof(colorshift_specs) / sizeof(colorshift_specs[0]),
    sizeof(ColorShiftConfig),
    colorshift_config_defaults,
    colorshift_free_config,
    colorshift_make,
};
