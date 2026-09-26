#include "effects/expand.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/graphics.h"

typedef struct {
    ExpandConfig config;
} Expand;

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

void expand_config_defaults(void *cfg_ptr) {
    ExpandConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    easing_parse("in_out_quart", &cfg->expand_easing);
    cfg->movement_speed = 0.35;
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "FFFFFF");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void expand_free_config(void *cfg_ptr) {
    ExpandConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int expand_build(Effect *self, EngineCtx *ctx) {
    Expand *st = self->state;
    ExpandConfig *cfg = &st->config;

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

    // The reference builds the final-color map in a first pass, then re-queries
    // the characters (RNG-neutral for this sort) and sets up motion.
    size_t ignored_n = 0;
    CharId *first_pass = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &ignored_n);
    free(first_pass);

    size_t n = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);

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

        Coord center = ctx->terminal.canvas.center;
        motion_set_coordinate(&ctx->terminal.arena.items[id].motion, center);
        char *path_id = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, cfg->movement_speed, true, cfg->expand_easing,
                            false, 0, 0, false, "", &path_id) != 0) {
            free(input_symbol);
            rc = -1;
            break;
        }
        Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, path_id);
        Waypoint wp;
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(p, input_coord, NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(path_id);
            free(input_symbol);
            rc = -1;
            break;
        }
        waypoint_free(&wp);

        terminal_set_character_visibility(&ctx->terminal, id, true);
        ac_insert(&ctx->active_characters, id);

        CallerKey caller;
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = path_id;
        EventAction set_layer;
        memset(&set_layer, 0, sizeof(set_layer));
        set_layer.kind = ACTION_SET_LAYER;
        set_layer.layer = 1;
        if (engine_register_event(ctx, id, EVENT_PATH_ACTIVATED, &caller, &set_layer) != 0) {
            free(path_id);
            free(input_symbol);
            rc = -1;
            break;
        }
        set_layer.layer = 0;
        if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &set_layer) != 0) {
            free(path_id);
            free(input_symbol);
            rc = -1;
            break;
        }

        engine_activate_path(ctx, self, id, path_id);
        free(path_id);

        ch = &ctx->terminal.arena.items[id];
        const char *scene_id = animation_new_scene(&ch->animation, false, true, SYNC_DISTANCE, false, no_ease, "",
                                                   uses_pre);
        Scene *scene = (Scene *)om_get(&ch->animation.scenes, scene_id);
        const char *symbols[1] = {input_symbol};

        if (dynamic) {
            if (has_input_fg || has_input_bg) {
                Gradient fg_grad;
                Gradient bg_grad;
                bool has_fg_grad = false;
                bool has_bg_grad = false;
                if (has_input_fg) {
                    Color stops[2] = {final_gradient.spectrum[0], input_fg};
                    if (gradient_with_steps(stops, 2, 10, false, &fg_grad) == 0) {
                        has_fg_grad = true;
                    } else {
                        rc = -1;
                    }
                }
                if (rc == 0 && has_input_bg) {
                    Color stops[2] = {final_gradient.spectrum[0], input_bg};
                    if (gradient_with_steps(stops, 2, 10, false, &bg_grad) == 0) {
                        has_bg_grad = true;
                    } else {
                        rc = -1;
                    }
                }
                if (rc == 0 &&
                    scene_apply_gradient_to_symbols(scene, symbols, 1, 1, has_fg_grad ? &fg_grad : NULL,
                                                    has_bg_grad ? &bg_grad : NULL) != 0) {
                    rc = -1;
                }
                if (has_fg_grad) {
                    gradient_free(&fg_grad);
                }
                if (has_bg_grad) {
                    gradient_free(&bg_grad);
                }
            } else {
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                if (scene_add_frame(scene, input_symbol, 1, &vp) != 0) {
                    rc = -1;
                }
            }
        } else {
            const Color *cc = coordcolormap_get(&mapping, input_coord);
            Color final_fg;
            memset(&final_fg, 0, sizeof(final_fg));
            if (cc) {
                final_fg = *cc;
            }
            Color stops[2] = {final_gradient.spectrum[0], final_fg};
            Gradient gradient;
            if (gradient_with_steps(stops, 2, 10, false, &gradient) != 0) {
                rc = -1;
            } else {
                if (scene_apply_gradient_to_symbols(scene, symbols, 1, 5, &gradient, NULL) != 0) {
                    rc = -1;
                }
                gradient_free(&gradient);
            }
        }
        if (rc == 0) {
            engine_activate_scene(ctx, self, id, scene_id);
        }
        free(input_symbol);
    }

    free(characters);
    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    return rc;
}

static const char *expand_next_frame(Effect *self, EngineCtx *ctx) {
    if (!ac_is_empty(&ctx->active_characters)) {
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void expand_destroy(Effect *self) {
    Expand *st = self->state;
    if (!st) {
        return;
    }
    free(st);
    free(self);
}

static const EffectOps EXPAND_OPS = {expand_build, expand_next_frame, expand_destroy, NULL};

Effect *expand_make(const void *cfg) {
    Expand *st = calloc(1, sizeof(Expand));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const ExpandConfig *)cfg;
    effect->ops = &EXPAND_OPS;
    effect->state = st;
    return effect;
}

const EffectEntry expand_entry = {
    "expand",
    (const EffOptSpec[]){
        EF_SPEC("expand-easing", 0, EF_EASING, offsetof(ExpandConfig, expand_easing)),
        EF_SPEC("movement-speed", 0, EF_FLOAT_POS, offsetof(ExpandConfig, movement_speed)),
        EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(ExpandConfig, final_gradient_stops)),
        EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(ExpandConfig, final_gradient_steps)),
        EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(ExpandConfig, final_gradient_direction)),
    },
    5,
    sizeof(ExpandConfig),
    expand_config_defaults,
    expand_free_config,
    expand_make,
};
