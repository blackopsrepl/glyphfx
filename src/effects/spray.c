#include "effects/spray.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/graphics.h"
#include "utils/pycompat.h"

typedef struct {
    SprayConfig config;
    CharId *pending;
    size_t pending_len;
    int64_t volume;
} Spray;

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

static int parse_spray_position(const char *value, void *dst) {
    SprayPosition *p = dst;
    if (strcmp(value, "n") == 0) {
        *p = SPRAY_N;
    } else if (strcmp(value, "ne") == 0) {
        *p = SPRAY_NE;
    } else if (strcmp(value, "e") == 0) {
        *p = SPRAY_E;
    } else if (strcmp(value, "se") == 0) {
        *p = SPRAY_SE;
    } else if (strcmp(value, "s") == 0) {
        *p = SPRAY_S;
    } else if (strcmp(value, "sw") == 0) {
        *p = SPRAY_SW;
    } else if (strcmp(value, "w") == 0) {
        *p = SPRAY_W;
    } else if (strcmp(value, "nw") == 0) {
        *p = SPRAY_NW;
    } else if (strcmp(value, "center") == 0) {
        *p = SPRAY_CENTER;
    } else {
        return -1;
    }
    return 0;
}

void spray_config_defaults(void *cfg_ptr) {
    SprayConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->spray_position = SPRAY_E;
    cfg->spray_volume = 0.005;
    cfg->movement_speed_range.start = 0.6;
    cfg->movement_speed_range.end = 1.4;
    easing_parse("out_expo", &cfg->movement_easing);
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "FFFFFF");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void spray_free_config(void *cfg_ptr) {
    SprayConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static ColorPair spray_final_colors(EngineCtx *ctx, CoordColorMap *mapping, bool dynamic, CharId id) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    ColorPair fc;
    memset(&fc, 0, sizeof(fc));
    if (dynamic) {
        fc.has_fg = ch->animation.has_input_fg;
        fc.fg = ch->animation.input_fg_color;
        fc.has_bg = ch->animation.has_input_bg;
        fc.bg = ch->animation.input_bg_color;
    } else {
        const Color *cc = coordcolormap_get(mapping, ch->input_coord);
        fc.has_fg = true;
        if (cc) {
            fc.fg = *cc;
        }
    }
    return fc;
}

static int spray_build(Effect *self, EngineCtx *ctx) {
    Spray *st = self->state;
    SprayConfig *cfg = &st->config;

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
    st->pending = malloc((n ? n : 1) * sizeof(CharId));

    const Canvas *canvas = &ctx->terminal.canvas;
    Coord spray_origin;
    switch (cfg->spray_position) {
        case SPRAY_CENTER:
            spray_origin = canvas->center;
            break;
        case SPRAY_N:
            spray_origin = coord_new(py_floor_div(canvas->right, 2), canvas->top);
            break;
        case SPRAY_NW:
            spray_origin = coord_new(canvas->left, canvas->top);
            break;
        case SPRAY_W:
            spray_origin = coord_new(canvas->left, py_floor_div(canvas->top, 2));
            break;
        case SPRAY_SW:
            spray_origin = coord_new(canvas->left, canvas->bottom);
            break;
        case SPRAY_S:
            spray_origin = coord_new(py_floor_div(canvas->right, 2), canvas->bottom);
            break;
        case SPRAY_SE:
            spray_origin = coord_new(canvas->right - 1, canvas->bottom);
            break;
        case SPRAY_E:
            spray_origin = coord_new(canvas->right - 1, py_floor_div(canvas->top, 2));
            break;
        case SPRAY_NE:
        default:
            spray_origin = coord_new(canvas->right - 1, canvas->top);
            break;
    }

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    int rc = 0;
    size_t pending_count = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        Coord input_coord = ch->input_coord;
        bool uses_pre = ch->uses_input_preexisting_colors;
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);

        double speed = rng_uniform(&ctx->rng, cfg->movement_speed_range.start, cfg->movement_speed_range.end);

        motion_set_coordinate(&ctx->terminal.arena.items[id].motion, spray_origin);
        char *path_id = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, speed, true, cfg->movement_easing, false, 0, 0,
                            false, "", &path_id) != 0) {
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

        ch = &ctx->terminal.arena.items[id];
        const char *scene_id = animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease,
                                                   "", uses_pre);
        Scene *scene = (Scene *)om_get(&ch->animation.scenes, scene_id);
        ColorPair final_colors = spray_final_colors(ctx, &mapping, dynamic, id);
        const char *symbols[1] = {input_symbol};

        if (dynamic) {
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors = final_colors;
            for (int frame = 0; frame < 7 && rc == 0; frame++) {
                if (scene_add_frame(scene, input_symbol, 20, &vp) != 0) {
                    rc = -1;
                }
            }
        } else {
            Color start_color = final_gradient.spectrum[rng_choice_index(&ctx->rng, final_gradient.len)];
            Color stops[2] = {start_color, final_colors.fg};
            Gradient spray_gradient;
            if (gradient_with_steps(stops, 2, 7, false, &spray_gradient) != 0) {
                rc = -1;
            } else {
                if (scene_apply_gradient_to_symbols(scene, symbols, 1, 20, &spray_gradient, NULL) != 0) {
                    rc = -1;
                }
                gradient_free(&spray_gradient);
            }
        }

        if (rc == 0) {
            engine_activate_scene(ctx, self, id, scene_id);
            engine_activate_path(ctx, self, id, path_id);
            st->pending[pending_count++] = id;
        }
        free(path_id);
        free(input_symbol);
    }

    st->pending_len = pending_count;
    rng_shuffle(&ctx->rng, st->pending, pending_count, sizeof(CharId));
    int64_t volume = (int64_t)((double)pending_count * cfg->spray_volume);
    st->volume = volume < 1 ? 1 : volume;

    free(characters);
    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    return rc;
}

static char *spray_next_frame(Effect *self, EngineCtx *ctx) {
    Spray *st = self->state;
    if (st->pending_len > 0 || !ac_is_empty(&ctx->active_characters)) {
        if (st->pending_len > 0) {
            int64_t count = rng_randint(&ctx->rng, 1, st->volume);
            for (int64_t i = 0; i < count; i++) {
                if (st->pending_len == 0) {
                    break;
                }
                CharId id = st->pending[--st->pending_len];
                terminal_set_character_visibility(&ctx->terminal, id, true);
                ac_insert(&ctx->active_characters, id);
            }
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void spray_destroy(Effect *self) {
    Spray *st = self->state;
    if (!st) {
        return;
    }
    free(st->pending);
    free(st);
    free(self);
}

static const EffectOps SPRAY_OPS = {spray_build, spray_next_frame, spray_destroy, NULL};

Effect *spray_make(const void *cfg) {
    Spray *st = calloc(1, sizeof(Spray));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const SprayConfig *)cfg;
    effect->ops = &SPRAY_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec spray_specs[] = {
    {"spray-position", 0, EF_CUSTOM, offsetof(SprayConfig, spray_position), parse_spray_position},
    EF_SPEC("spray-volume", 0, EF_RATIO_POS, offsetof(SprayConfig, spray_volume)),
    EF_SPEC("movement-speed-range", 0, EF_FLOAT_RANGE, offsetof(SprayConfig, movement_speed_range)),
    EF_SPEC("movement-easing", 0, EF_EASING, offsetof(SprayConfig, movement_easing)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(SprayConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(SprayConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(SprayConfig, final_gradient_direction)),
};

const EffectEntry spray_entry = {
    "spray",
    spray_specs,
    sizeof(spray_specs) / sizeof(spray_specs[0]),
    sizeof(SprayConfig),
    spray_config_defaults,
    spray_free_config,
    spray_make,
};
