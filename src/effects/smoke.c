// smoke, ported from the reference effects/smoke.rs.
#include "effects/smoke.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/canvas.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/easing.h"
#include "utils/graphics.h"
#include "utils/ordmap.h"
#include "utils/spanning_tree.h"

typedef struct {
    SmokeConfig config;
    ColorPair *character_final_color_map;
    bool *final_present;
    size_t map_len;
    BreadthFirst fill_alg;
    bool has_fill_alg;
    Scene *smoke_template;  // borrowed: first shareable smoke scene
    FrameMemo *paint_runs;
} Smoke;

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

static Easing no_ease(void) {
    Easing e;
    memset(&e, 0, sizeof(e));
    return e;
}

void smoke_config_defaults(void *cfg_ptr) {
    SmokeConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    color_from_hex("7A7A7A", &cfg->starting_color);
    push_symbol_default(&cfg->smoke_symbols, "░");
    push_symbol_default(&cfg->smoke_symbols, "▒");
    push_symbol_default(&cfg->smoke_symbols, "▓");
    push_symbol_default(&cfg->smoke_symbols, "▒");
    push_symbol_default(&cfg->smoke_symbols, "░");
    push_color_default(&cfg->smoke_gradient_stops, "242424");
    push_color_default(&cfg->smoke_gradient_stops, "FFFFFF");
    cfg->use_whole_canvas = false;
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "FFFFFF");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void smoke_free_config(void *cfg_ptr) {
    SmokeConfig *cfg = cfg_ptr;
    for (size_t i = 0; i < cfg->smoke_symbols.len; i++) {
        free(cfg->smoke_symbols.items[i]);
    }
    free(cfg->smoke_symbols.items);
    free(cfg->smoke_gradient_stops.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int smoke_build(Effect *self, EngineCtx *ctx) {
    Smoke *st = self->state;
    SmokeConfig *cfg = &st->config;

    // SmokeIterator.__init__ order: PrimsWeighted (random starting coord +
    // per-character weights), then the BreadthFirst starting character.
    bool limit_to_text_boundary = !cfg->use_whole_canvas;
    PrimsWeighted gen_alg;
    if (prims_weighted_new(&gen_alg, ctx, false, CHAR_ID_NONE, limit_to_text_boundary) != 0) {
        return -1;
    }
    Coord fill_start_coord = canvas_random_coord(&ctx->terminal.canvas, &ctx->rng, false, limit_to_text_boundary);
    CharId fill_start_char = terminal_get_character_by_input_coord(&ctx->terminal, fill_start_coord);
    bool has_start = fill_start_char != CHAR_ID_NONE;
    if (breadth_first_new(&st->fill_alg, ctx, has_start, fill_start_char, limit_to_text_boundary) != 0) {
        prims_weighted_free(&gen_alg);
        return -1;
    }
    st->has_fill_alg = true;

    // SmokeIterator.build()
    Gradient final_gradient;
    if (gradient_new(cfg->final_gradient_stops.items, cfg->final_gradient_stops.len,
                     cfg->final_gradient_steps.items, cfg->final_gradient_steps.len, false, false,
                     &final_gradient) != 0) {
        prims_weighted_free(&gen_alg);
        return -1;
    }
    CoordColorMap final_gradient_mapping;
    if (gradient_build_coordinate_color_mapping(&final_gradient, ctx->terminal.canvas.text_bottom,
                                                ctx->terminal.canvas.text_top, ctx->terminal.canvas.text_left,
                                                ctx->terminal.canvas.text_right,
                                                cfg->final_gradient_direction, &final_gradient_mapping) != 0) {
        gradient_free(&final_gradient);
        prims_weighted_free(&gen_alg);
        return -1;
    }
    Color blk;
    color_from_hex("000000", &blk);

    // Gradient(*smoke_gradient_stops, *final_gradient_stops[::-1], steps=(3, 4))
    size_t smoke_colors_len = cfg->smoke_gradient_stops.len + cfg->final_gradient_stops.len;
    Color *smoke_colors = malloc((smoke_colors_len ? smoke_colors_len : 1) * sizeof(Color));
    size_t sc = 0;
    for (size_t i = 0; i < cfg->smoke_gradient_stops.len; i++) {
        smoke_colors[sc++] = cfg->smoke_gradient_stops.items[i];
    }
    for (size_t i = cfg->final_gradient_stops.len; i > 0; i--) {
        smoke_colors[sc++] = cfg->final_gradient_stops.items[i - 1];
    }
    int64_t smoke_steps[2] = {3, 4};
    Gradient smoke_gradient;
    if (gradient_new(smoke_colors, smoke_colors_len, smoke_steps, 2, false, false, &smoke_gradient) != 0) {
        free(smoke_colors);
        coordcolormap_free(&final_gradient_mapping);
        gradient_free(&final_gradient);
        prims_weighted_free(&gen_alg);
        return -1;
    }
    free(smoke_colors);

    st->paint_runs = framemo_new();

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    CharacterFilter filter = {true, true, true, false};
    size_t chars_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, filter,
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &chars_len);
    st->map_len = ctx->terminal.arena.len;
    st->character_final_color_map = calloc(st->map_len ? st->map_len : 1, sizeof(ColorPair));
    st->final_present = calloc(st->map_len ? st->map_len : 1, sizeof(bool));
    Easing ease = no_ease();

    int rc = 0;
    for (size_t i = 0; i < chars_len && rc == 0; i++) {
        CharId id = characters[i];
        terminal_set_character_visibility(&ctx->terminal, id, true);
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        Coord input_coord = ch->input_coord;
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);
        bool has_input_fg = ch->animation.has_input_fg;
        Color input_fg = ch->animation.input_fg_color;
        bool has_input_bg = ch->animation.has_input_bg;
        Color input_bg = ch->animation.input_bg_color;
        bool uses_pre = ch->uses_input_preexisting_colors;

        ColorPair map_colors;
        ColorPair base_colors;
        memset(&map_colors, 0, sizeof(map_colors));
        memset(&base_colors, 0, sizeof(base_colors));
        if (dynamic) {
            map_colors.has_fg = has_input_fg;
            map_colors.fg = input_fg;
            map_colors.has_bg = has_input_bg;
            map_colors.bg = input_bg;
            base_colors.has_fg = true;
            base_colors.fg = blk;
        } else {
            const Color *mapped = coordcolormap_get(&final_gradient_mapping, input_coord);
            map_colors.has_fg = true;
            map_colors.fg = mapped ? *mapped : blk;
            base_colors.has_fg = true;
            base_colors.fg = cfg->starting_color;
        }
        if ((size_t)id < st->map_len) {
            st->character_final_color_map[id] = map_colors;
            st->final_present[id] = true;
        }

        const char *paint_chars[1] = {input_symbol};
        const char *paint_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                    SYNC_DISTANCE, false, ease, "paint", uses_pre);
        if (dynamic) {
            Scene *scene =
                paint_scn ? (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, paint_scn) : NULL;
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors = map_colors;
            if (!scene || scene_add_frame(scene, input_symbol, 5, &vp) != 0) {
                rc = -1;
            }
        } else {
            Color final_fg = map_colors.fg;
            Scene *scene =
                paint_scn ? (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, paint_scn) : NULL;
            bool shareable = scene && !scene->has_preexisting_colors && !scene->preexisting_bold;
            Scene *run = shareable ? framemo_get(st->paint_runs, input_symbol, final_fg) : NULL;
            if (run) {
                if (scene_append_frames(scene, run) != 0) {
                    rc = -1;
                }
            } else if (scene) {
                size_t paint_len = cfg->final_gradient_stops.len + 1;
                Color *paint_stops = malloc((paint_len ? paint_len : 1) * sizeof(Color));
                size_t pi = 0;
                for (size_t j = 0; j < cfg->final_gradient_stops.len; j++) {
                    paint_stops[pi++] = cfg->final_gradient_stops.items[j];
                }
                paint_stops[pi++] = final_fg;
                Gradient paint_gradient;
                if (gradient_with_steps(paint_stops, paint_len, 5, false, &paint_gradient) != 0) {
                    rc = -1;
                } else {
                    if (scene_apply_gradient_to_symbols(scene, paint_chars, 1, 5, &paint_gradient, NULL) != 0) {
                        rc = -1;
                    } else if (shareable && framemo_put(st->paint_runs, input_symbol, final_fg, scene) != 0) {
                        rc = -1;
                    }
                    gradient_free(&paint_gradient);
                }
                free(paint_stops);
            } else {
                rc = -1;
            }
        }
        if (rc != 0) {
            free(input_symbol);
            break;
        }

        const char *smoke_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                    SYNC_DISTANCE, false, ease, "smoke", uses_pre);
        Scene *smoke_scene =
            smoke_scn ? (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, smoke_scn) : NULL;
        if (dynamic) {
            for (size_t j = 0; j < cfg->smoke_symbols.len && rc == 0; j++) {
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                vp.colors = map_colors;
                if (!smoke_scene || scene_add_frame(smoke_scene, cfg->smoke_symbols.items[j], 10, &vp) != 0) {
                    rc = -1;
                }
            }
        } else if (smoke_scene && !smoke_scene->has_preexisting_colors && !smoke_scene->preexisting_bold) {
            // The smoke frames are identical for every character: the first
            // shareable scene applies the gradient and becomes the template;
            // the rest copy its frames (the asm engine keeps one template too).
            if (st->smoke_template) {
                if (scene_append_frames(smoke_scene, st->smoke_template) != 0) {
                    rc = -1;
                }
            } else {
                if (scene_apply_gradient_to_symbols(smoke_scene,
                                                    (const char *const *)cfg->smoke_symbols.items,
                                                    cfg->smoke_symbols.len, 3, &smoke_gradient, NULL) != 0) {
                    rc = -1;
                }
                st->smoke_template = smoke_scene;
            }
        } else {
            if (!smoke_scene ||
                scene_apply_gradient_to_symbols(smoke_scene,
                                                (const char *const *)cfg->smoke_symbols.items,
                                                cfg->smoke_symbols.len, 3, &smoke_gradient, NULL) != 0) {
                rc = -1;
            }
        }
        if (rc == 0) {
            CallerKey caller;
            memset(&caller, 0, sizeof(caller));
            caller.kind = CALLER_SCENE;
            caller.id = (char *)smoke_scn;
            EventAction action;
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_ACTIVATE_SCENE;
            action.has_id = true;
            action.id = (char *)paint_scn;
            if (engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
                rc = -1;
            }
        }
        if (rc == 0) {
            animation_set_appearance(&ctx->terminal.arena.items[id].animation, uses_pre, input_symbol,
                                     &base_colors);
        }
        free(input_symbol);
    }
    free(characters);

    while (!gen_alg.complete) {
        prims_weighted_step(&gen_alg, ctx);
    }

    // trigger effects on starting char since it will not be 'explored'
    CharId starting_char = st->fill_alg.starting_char;
    engine_activate_scene(ctx, self, starting_char, "smoke");
    ac_insert(&ctx->active_characters, starting_char);

    gradient_free(&smoke_gradient);
    coordcolormap_free(&final_gradient_mapping);
    gradient_free(&final_gradient);
    prims_weighted_free(&gen_alg);
    return rc;
}

static const char *smoke_next_frame(Effect *self, EngineCtx *ctx) {
    Smoke *st = self->state;
    BreadthFirst *fill_alg = &st->fill_alg;
    if (!fill_alg->complete || !ac_is_empty(&ctx->active_characters)) {
        if (!fill_alg->complete) {
            breadth_first_step(fill_alg, ctx);
            for (size_t i = 0; i < fill_alg->explored_last_step_len; i++) {
                CharId id = fill_alg->explored_last_step[i];
                engine_activate_scene(ctx, self, id, "smoke");
                ac_insert(&ctx->active_characters, id);
            }
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void smoke_callback(Effect *self, EngineCtx *ctx, CharId character, const EffectCallback *cb) {
    (void)self;
    (void)ctx;
    (void)character;
    (void)cb;
}

static void smoke_destroy(Effect *self) {
    Smoke *st = self->state;
    if (!st) {
        return;
    }
    if (st->has_fill_alg) {
        breadth_first_free(&st->fill_alg);
    }
    framemo_free(st->paint_runs);
    free(st->character_final_color_map);
    free(st->final_present);
    free(st);
    free(self);
}

static const EffectOps SMOKE_OPS = {smoke_build, smoke_next_frame, smoke_destroy, smoke_callback};

Effect *smoke_make(const void *cfg) {
    Smoke *st = calloc(1, sizeof(Smoke));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const SmokeConfig *)cfg;
    effect->ops = &SMOKE_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec smoke_specs[] = {
    EF_SPEC("starting-color", 0, EF_COLOR, offsetof(SmokeConfig, starting_color)),
    {"smoke-symbols", 0, EF_STRING_LIST, offsetof(SmokeConfig, smoke_symbols), NULL},
    EF_SPEC("smoke-gradient-stops", 0, EF_COLOR_LIST, offsetof(SmokeConfig, smoke_gradient_stops)),
    EF_SPEC("use-whole-canvas", 0, EF_FLAG, offsetof(SmokeConfig, use_whole_canvas)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(SmokeConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(SmokeConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(SmokeConfig, final_gradient_direction)),
};

const EffectEntry smoke_entry = {
    "smoke",
    smoke_specs,
    sizeof(smoke_specs) / sizeof(smoke_specs[0]),
    sizeof(SmokeConfig),
    smoke_config_defaults,
    smoke_free_config,
    smoke_make,
};
