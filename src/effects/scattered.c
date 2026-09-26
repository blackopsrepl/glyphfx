#include "effects/scattered.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/canvas.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/graphics.h"

typedef struct {
    ScatteredConfig config;
    int64_t initial_hold_frames;
} Scattered;

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

void scattered_config_defaults(void *cfg_ptr) {
    ScatteredConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->movement_speed = 0.5;
    easing_parse("in_out_back", &cfg->movement_easing);
    push_color_default(&cfg->final_gradient_stops, "ff9048");
    push_color_default(&cfg->final_gradient_stops, "ab9dff");
    push_color_default(&cfg->final_gradient_stops, "bdffea");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_frames = 9;
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void scattered_free_config(void *cfg_ptr) {
    ScatteredConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int scattered_build(Effect *self, EngineCtx *ctx) {
    Scattered *st = self->state;
    ScatteredConfig *cfg = &st->config;

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

        ColorPair final_colors;
        memset(&final_colors, 0, sizeof(final_colors));
        if (dynamic) {
            final_colors.has_fg = has_input_fg;
            final_colors.fg = input_fg;
            final_colors.has_bg = has_input_bg;
            final_colors.bg = input_bg;
        } else {
            const Color *cc = coordcolormap_get(&mapping, input_coord);
            final_colors.has_fg = true;
            if (cc) {
                final_colors.fg = *cc;
            }
        }

        Coord start_coord;
        if (ctx->terminal.canvas.right < 2 || ctx->terminal.canvas.top < 2) {
            start_coord = coord_new(1, 1);
        } else {
            start_coord = canvas_random_coord(&ctx->terminal.canvas, &ctx->rng, false, false);
        }

        motion_set_coordinate(&ctx->terminal.arena.items[id].motion, start_coord);
        char *path_id = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, cfg->movement_speed, true,
                            cfg->movement_easing, false, 0, 0, false, "", &path_id) != 0) {
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
        terminal_set_character_visibility(&ctx->terminal, id, true);

        ch = &ctx->terminal.arena.items[id];
        const char *scene_id = animation_new_scene(&ch->animation, false, true, SYNC_DISTANCE, false, no_ease, "",
                                                   uses_pre);
        Scene *scene = (Scene *)om_get(&ch->animation.scenes, scene_id);
        const char *symbols[1] = {input_symbol};
        if (dynamic) {
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors = final_colors;
            if (scene_add_frame(scene, input_symbol, cfg->final_gradient_frames, &vp) != 0) {
                rc = -1;
            }
        } else {
            Color stops[2] = {final_gradient.spectrum[0], final_colors.fg};
            Gradient char_gradient;
            if (gradient_with_steps(stops, 2, 10, false, &char_gradient) != 0) {
                rc = -1;
            } else {
                if (scene_apply_gradient_to_symbols(scene, symbols, 1, cfg->final_gradient_frames, &char_gradient,
                                                    NULL) != 0) {
                    rc = -1;
                }
                gradient_free(&char_gradient);
            }
        }
        if (rc == 0) {
            engine_activate_scene(ctx, self, id, scene_id);
        }
        ac_insert(&ctx->active_characters, id);
        free(input_symbol);
    }

    free(characters);
    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    st->initial_hold_frames = 25;
    return rc;
}

static const char *scattered_next_frame(Effect *self, EngineCtx *ctx) {
    Scattered *st = self->state;
    if (!ac_is_empty(&ctx->active_characters)) {
        if (st->initial_hold_frames != 0) {
            st->initial_hold_frames -= 1;
            return engine_frame(ctx);
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void scattered_destroy(Effect *self) {
    Scattered *st = self->state;
    if (!st) {
        return;
    }
    free(st);
    free(self);
}

static const EffectOps SCATTERED_OPS = {scattered_build, scattered_next_frame, scattered_destroy, NULL};

Effect *scattered_make(const void *cfg) {
    Scattered *st = calloc(1, sizeof(Scattered));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const ScatteredConfig *)cfg;
    effect->ops = &SCATTERED_OPS;
    effect->state = st;
    return effect;
}

const EffectEntry scattered_entry = {
    "scattered",
    (const EffOptSpec[]){
        EF_SPEC("movement-speed", 0, EF_FLOAT_POS, offsetof(ScatteredConfig, movement_speed)),
        EF_SPEC("movement-easing", 0, EF_EASING, offsetof(ScatteredConfig, movement_easing)),
        EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(ScatteredConfig, final_gradient_stops)),
        EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(ScatteredConfig, final_gradient_steps)),
        EF_SPEC("final-gradient-frames", 0, EF_INT, offsetof(ScatteredConfig, final_gradient_frames)),
        EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(ScatteredConfig, final_gradient_direction)),
    },
    6,
    sizeof(ScatteredConfig),
    scattered_config_defaults,
    scattered_free_config,
    scattered_make,
};
