// burn, ported from the reference effects/burn.rs.
#include "effects/burn.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/motion.h"
#include "engine/particles.h"
#include "engine/terminal.h"
#include "utils/easing.h"
#include "utils/graphics.h"
#include "utils/ordmap.h"
#include "utils/spanning_tree.h"

/// Callback id: EventHandler.Callback(lambda c: self._emit_smoke(...)).
#define CB_EMIT_SMOKE 0u
/// Callback id: ParticlePool.reclaim_on_event's reclaim closure.
#define CB_RECLAIM_SMOKE 1u

typedef struct {
    BurnConfig config;
    Color *character_final_color_map;
    bool *final_present;
    size_t map_len;
    /// PrimsSimple.char_link_order, consumed FIFO in next_frame.
    CharId *char_link_order;
    size_t char_link_order_len;
    size_t char_link_order_head;
    ParticlePool smoke_particles;
    bool has_smoke_particles;
    int64_t emission_counter;
} Burn;

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

static Easing no_ease(void) {
    Easing e;
    memset(&e, 0, sizeof(e));
    return e;
}

void burn_config_defaults(void *cfg_ptr) {
    BurnConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    color_from_hex("837373", &cfg->starting_color);
    push_color_default(&cfg->burn_colors, "ffffff");
    push_color_default(&cfg->burn_colors, "fff75d");
    push_color_default(&cfg->burn_colors, "fe650d");
    push_color_default(&cfg->burn_colors, "8A003C");
    push_color_default(&cfg->burn_colors, "510100");
    cfg->smoke_chance = 0.5;
    push_color_default(&cfg->final_gradient_stops, "00c3ff");
    push_color_default(&cfg->final_gradient_stops, "ffff1c");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void burn_free_config(void *cfg_ptr) {
    BurnConfig *cfg = cfg_ptr;
    free(cfg->burn_colors.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

/// BurnIterator._make_smoke_pool's initialize_smoke: one reusable "smoke"
/// scene (10-frame 504F4F->C7C7C7 fade) and layer 2.
static void burn_initialize_smoke(void *user, EngineCtx *ctx, CharId id) {
    (void)user;
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    size_t sym_len = strlen(ch->input_symbol);
    char *input_symbol = malloc(sym_len + 1);
    strcpy(input_symbol, ch->input_symbol);
    bool uses_pre = ch->uses_input_preexisting_colors;

    Color stops[2];
    color_from_hex("504F4F", &stops[0]);
    color_from_hex("C7C7C7", &stops[1]);
    Gradient gradient;
    if (gradient_with_steps(stops, 2, 9, false, &gradient) != 0) {
        free(input_symbol);
        return;
    }
    Easing ease = no_ease();
    ch = &ctx->terminal.arena.items[id];
    const char *smoke_scn = animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, ease, "smoke",
                                                uses_pre);
    Scene *scene = smoke_scn ? (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, smoke_scn) : NULL;
    if (scene) {
        for (size_t i = 0; i < gradient.len; i++) {
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = gradient.spectrum[i];
            scene_add_frame(scene, input_symbol, 10, &vp);
        }
    }
    ctx->terminal.arena.items[id].layer = 2;
    gradient_free(&gradient);
    free(input_symbol);
}

/// BurnIterator._emit_smoke.
typedef struct {
    Effect *self;
    Burn *st;
    Coord origin;
} BurnEmit;

/// emit's on_emit closure.
static void burn_on_emit(void *user, EngineCtx *ctx, CharId particle) {
    BurnEmit *e = user;
    EffectCharacter *ch = &ctx->terminal.arena.items[particle];
    Scene *scene = (Scene *)om_get(&ch->animation.scenes, "smoke");
    if (scene) {
        scene_reset(scene);
    }
    char *smoke_path = NULL;
    Easing ease = no_ease();
    if (motion_new_path(&ctx->terminal.arena.items[particle].motion, 0.5, false, ease, false, 0, 0, false, "",
                        &smoke_path) != 0) {
        return;
    }
    int64_t rise_column = rng_randint(&ctx->rng, e->origin.column - 4, e->origin.column + 4);
    Coord rise_target_coord = coord_new(rise_column, ctx->terminal.canvas.top + 1);
    Path *p = (Path *)om_get(&ctx->terminal.arena.items[particle].motion.paths, smoke_path);
    Waypoint wp;
    memset(&wp, 0, sizeof(wp));
    (void)path_new_waypoint(p, rise_target_coord, NULL, 0, "", &wp);
    waypoint_free(&wp);
    engine_activate_path(ctx, e->self, particle, smoke_path);
    engine_activate_scene(ctx, e->self, particle, "smoke");

    CallerKey caller;
    memset(&caller, 0, sizeof(caller));
    caller.kind = CALLER_SCENE;
    caller.id = "smoke";
    EventAction action;
    memset(&action, 0, sizeof(action));
    action.kind = ACTION_CALLBACK;
    action.cb.id = CB_RECLAIM_SMOKE;
    (void)engine_register_event(ctx, particle, EVENT_SCENE_COMPLETE, &caller, &action);
    free(smoke_path);
}

static void burn_emit_smoke(Effect *self, EngineCtx *ctx, Coord origin) {
    Burn *st = self->state;
    if (rng_random(&ctx->rng) > st->config.smoke_chance) {
        return;
    }
    st->emission_counter += 1;
    BurnEmit emit_ctx;
    emit_ctx.self = self;
    emit_ctx.st = st;
    emit_ctx.origin = origin;
    ParticleReset reset = particle_reset_default();
    particle_pool_emit(&st->smoke_particles, ctx, origin, NULL, true, reset, burn_initialize_smoke, st,
                       burn_on_emit, &emit_ctx);
}

static int burn_build(Effect *self, EngineCtx *ctx) {
    Burn *st = self->state;
    BurnConfig *cfg = &st->config;

    // BurnIterator.__init__ order: PrimsSimple (random starting coord), then
    // the pool preallocation (2000 choice() draws + initializer each).
    PrimsSimple algo;
    if (prims_simple_new(&algo, ctx, false, CHAR_ID_NONE, true) != 0) {
        return -1;
    }
    static const char *smoke_symbols[6] = {".", ",", "'", "`", "#", "*"};
    if (particle_pool_init(&st->smoke_particles, smoke_symbols, 6, true, 2000, coord_new(0, 0)) != 0) {
        prims_simple_free(&algo);
        return -1;
    }
    st->has_smoke_particles = true;
    if (particle_pool_preallocate(&st->smoke_particles, ctx, 2000, burn_initialize_smoke, st) != 0) {
        prims_simple_free(&algo);
        return -1;
    }

    static const char *burn_char_order[9] = {"'", ".", "▖", "▙", "█", "▜", "▀", "▝", "."};

    Gradient final_gradient;
    if (gradient_new(cfg->final_gradient_stops.items, cfg->final_gradient_stops.len,
                     cfg->final_gradient_steps.items, cfg->final_gradient_steps.len, false, false,
                     &final_gradient) != 0) {
        prims_simple_free(&algo);
        return -1;
    }
    CoordColorMap final_gradient_mapping;
    if (gradient_build_coordinate_color_mapping(&final_gradient, ctx->terminal.canvas.text_bottom,
                                                ctx->terminal.canvas.text_top, ctx->terminal.canvas.text_left,
                                                ctx->terminal.canvas.text_right,
                                                cfg->final_gradient_direction, &final_gradient_mapping) != 0) {
        gradient_free(&final_gradient);
        prims_simple_free(&algo);
        return -1;
    }

    CharacterFilter filter = character_filter_default();
    size_t chars_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, filter,
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &chars_len);
    st->map_len = ctx->terminal.arena.len;
    st->character_final_color_map = calloc(st->map_len ? st->map_len : 1, sizeof(Color));
    st->final_present = calloc(st->map_len ? st->map_len : 1, sizeof(bool));
    for (size_t i = 0; i < chars_len; i++) {
        CharId id = characters[i];
        Coord coord = ctx->terminal.arena.items[id].input_coord;
        const Color *mapped = coordcolormap_get(&final_gradient_mapping, coord);
        if ((size_t)id < st->map_len && mapped) {
            st->character_final_color_map[id] = *mapped;
            st->final_present[id] = true;
        }
    }
    free(characters);

    Gradient fire_gradient;
    if (gradient_with_steps(cfg->burn_colors.items, cfg->burn_colors.len, 10, false, &fire_gradient) != 0) {
        coordcolormap_free(&final_gradient_mapping);
        gradient_free(&final_gradient);
        prims_simple_free(&algo);
        return -1;
    }

    while (!algo.complete) {
        prims_simple_step(&algo, ctx);
    }

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    characters = terminal_get_characters(&ctx->terminal, &ctx->rng, filter, CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT,
                                         &chars_len);
    Easing ease = no_ease();
    int rc = 0;
    for (size_t i = 0; i < chars_len && rc == 0; i++) {
        CharId id = characters[i];
        terminal_set_character_visibility(&ctx->terminal, id, true);
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);
        bool has_input_fg = ch->animation.has_input_fg;
        Color input_fg = ch->animation.input_fg_color;
        bool has_input_bg = ch->animation.has_input_bg;
        Color input_bg = ch->animation.input_bg_color;
        bool uses_pre = ch->uses_input_preexisting_colors;

        ColorPair appearance;
        memset(&appearance, 0, sizeof(appearance));
        appearance.has_fg = true;
        appearance.fg = cfg->starting_color;
        animation_set_appearance(&ctx->terminal.arena.items[id].animation, uses_pre, input_symbol, &appearance);

        const char *burn_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                   SYNC_DISTANCE, false, ease, "burn", uses_pre);
        Scene *scene = burn_scn ? (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, burn_scn) : NULL;
        if (!scene || scene_apply_gradient_to_symbols(scene, burn_char_order, 9, 4, &fire_gradient, NULL) != 0) {
            rc = -1;
            free(input_symbol);
            break;
        }

        const char *final_color_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                          SYNC_DISTANCE, false, ease, "", uses_pre);
        Color fire_last = fire_gradient.spectrum[fire_gradient.len - 1];
        if (dynamic) {
            Gradient fg_gradient;
            Gradient bg_gradient;
            bool has_fg_gradient = false;
            bool has_bg_gradient = false;
            Color fstops[2] = {fire_last, input_fg};
            if (has_input_fg) {
                if (gradient_with_steps(fstops, 2, 8, false, &fg_gradient) != 0) {
                    rc = -1;
                } else {
                    has_fg_gradient = true;
                }
            }
            Color bstops[2] = {fire_last, input_bg};
            if (rc == 0 && has_input_bg) {
                if (gradient_with_steps(bstops, 2, 8, false, &bg_gradient) != 0) {
                    rc = -1;
                } else {
                    has_bg_gradient = true;
                }
            }
            if (rc == 0) {
                scene = final_color_scn
                            ? (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, final_color_scn)
                            : NULL;
                if (has_fg_gradient || has_bg_gradient) {
                    const char *syms[1] = {input_symbol};
                    if (!scene || scene_apply_gradient_to_symbols(scene, syms, 1, 4,
                                                                 has_fg_gradient ? &fg_gradient : NULL,
                                                                 has_bg_gradient ? &bg_gradient : NULL) != 0) {
                        rc = -1;
                    }
                } else {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    if (!scene || scene_add_frame(scene, input_symbol, 4, &vp) != 0) {
                        rc = -1;
                    }
                }
            }
            if (has_fg_gradient) {
                gradient_free(&fg_gradient);
            }
            if (has_bg_gradient) {
                gradient_free(&bg_gradient);
            }
        } else {
            Color final_color = st->character_final_color_map[id];
            Color cstops[2] = {fire_last, final_color};
            Gradient char_gradient;
            if (gradient_with_steps(cstops, 2, 8, false, &char_gradient) != 0) {
                rc = -1;
            } else {
                scene = final_color_scn
                            ? (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, final_color_scn)
                            : NULL;
                if (!scene) {
                    rc = -1;
                }
                for (size_t j = 0; j < char_gradient.len && rc == 0; j++) {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    vp.colors.has_fg = true;
                    vp.colors.fg = char_gradient.spectrum[j];
                    if (scene_add_frame(scene, input_symbol, 4, &vp) != 0) {
                        rc = -1;
                    }
                }
                gradient_free(&char_gradient);
            }
        }

        if (rc == 0) {
            CallerKey caller;
            memset(&caller, 0, sizeof(caller));
            caller.kind = CALLER_SCENE;
            caller.id = (char *)burn_scn;
            EventAction action;
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_ACTIVATE_SCENE;
            action.has_id = true;
            action.id = (char *)final_color_scn;
            if (engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
                rc = -1;
            }
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_CALLBACK;
            action.cb.id = CB_EMIT_SMOKE;
            if (rc == 0 && engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
                rc = -1;
            }
        }
        free(input_symbol);
    }
    free(characters);
    gradient_free(&fire_gradient);
    coordcolormap_free(&final_gradient_mapping);
    gradient_free(&final_gradient);

    if (rc != 0) {
        prims_simple_free(&algo);
        return -1;
    }
    st->char_link_order = algo.char_link_order;
    st->char_link_order_len = algo.char_link_order_len;
    st->char_link_order_head = 0;
    algo.char_link_order = NULL;
    prims_simple_free(&algo);
    return 0;
}

/// BurnIterator._has_input_colors.
static bool burn_has_input_colors(EngineCtx *ctx, CharId id) {
    Animation *anim = &ctx->terminal.arena.items[id].animation;
    return anim->has_input_fg || anim->has_input_bg;
}

/// BurnIterator._is_burnable.
static bool burn_is_burnable(EngineCtx *ctx, CharId id) {
    const char *symbol = ctx->terminal.arena.items[id].input_symbol;
    if (strcmp(symbol, " ") != 0) {
        return true;
    }
    return ctx->terminal.config.existing_color_handling != EXISTING_COLOR_IGNORE &&
           burn_has_input_colors(ctx, id);
}

static const char *burn_next_frame(Effect *self, EngineCtx *ctx) {
    Burn *st = self->state;
    bool has_pending = st->char_link_order_head < st->char_link_order_len;
    if (has_pending || !ac_is_empty(&ctx->active_characters)) {
        int64_t draws = rng_randint(&ctx->rng, 2, 4);
        for (int64_t i = 0; i < draws; i++) {
            if (st->char_link_order_head < st->char_link_order_len) {
                CharId next_char = st->char_link_order[st->char_link_order_head++];
                if (!burn_is_burnable(ctx, next_char)) {
                    continue;
                }
                engine_activate_scene(ctx, self, next_char, "burn");
                ac_insert(&ctx->active_characters, next_char);
            }
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void burn_callback(Effect *self, EngineCtx *ctx, CharId character, const EffectCallback *cb) {
    Burn *st = self->state;
    if (cb->id == CB_EMIT_SMOKE) {
        Coord origin = ctx->terminal.arena.items[character].input_coord;
        burn_emit_smoke(self, ctx, origin);
    } else if (cb->id == CB_RECLAIM_SMOKE) {
        particle_pool_reclaim(&st->smoke_particles, ctx, character, true, true);
    }
}

static void burn_destroy(Effect *self) {
    Burn *st = self->state;
    if (!st) {
        return;
    }
    if (st->has_smoke_particles) {
        particle_pool_free(&st->smoke_particles);
    }
    free(st->character_final_color_map);
    free(st->final_present);
    free(st->char_link_order);
    free(st);
    free(self);
}

static const EffectOps BURN_OPS = {burn_build, burn_next_frame, burn_destroy, burn_callback};

Effect *burn_make(const void *cfg) {
    Burn *st = calloc(1, sizeof(Burn));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const BurnConfig *)cfg;
    effect->ops = &BURN_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec burn_specs[] = {
    EF_SPEC("starting-color", 0, EF_COLOR, offsetof(BurnConfig, starting_color)),
    EF_SPEC("burn-colors", 0, EF_COLOR_LIST, offsetof(BurnConfig, burn_colors)),
    EF_SPEC("smoke-chance", 0, EF_RATIO_NONNEG, offsetof(BurnConfig, smoke_chance)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(BurnConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(BurnConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(BurnConfig, final_gradient_direction)),
};

const EffectEntry burn_entry = {
    "burn",
    burn_specs,
    sizeof(burn_specs) / sizeof(burn_specs[0]),
    sizeof(BurnConfig),
    burn_config_defaults,
    burn_free_config,
    burn_make,
};
