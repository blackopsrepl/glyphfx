#include "effects/errorcorrect.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/graphics.h"

static const char *const BLOCK_WIPE_START[8] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
static const char *const BLOCK_WIPE_END[7] = {"▇", "▆", "▅", "▄", "▃", "▂", "▁"};

typedef struct {
    CharId a;
    CharId b;
} SwapPair;

typedef struct {
    ErrorCorrectConfig config;
    SwapPair *swapped;
    size_t swapped_len;
    int64_t swap_delay;
    // Dense CharId -> final colors (the reference HashMap is only read by key).
    ColorPair *final_colors;
    bool *final_colors_present;
    size_t final_colors_len;
} ErrorCorrect;

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

void errorcorrect_config_defaults(void *cfg_ptr) {
    ErrorCorrectConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->error_pairs = 0.1;
    cfg->swap_delay = 6;
    color_from_hex("e74c3c", &cfg->error_color);
    color_from_hex("45bf55", &cfg->correct_color);
    cfg->movement_speed = 0.9;
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "FFFFFF");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void errorcorrect_free_config(void *cfg_ptr) {
    ErrorCorrectConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

// ErrorCorrectIterator._get_dynamic_final_scene.
static const char *errorcorrect_dynamic_final_scene(EngineCtx *ctx, ErrorCorrectConfig *cfg, CharId id) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
    strcpy(input_symbol, ch->input_symbol);
    bool has_input_fg = ch->animation.has_input_fg;
    Color input_fg = ch->animation.input_fg_color;
    bool has_input_bg = ch->animation.has_input_bg;
    Color input_bg = ch->animation.input_bg_color;
    bool uses_pre = ch->uses_input_preexisting_colors;

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    const char *final_scene =
        animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "", uses_pre);

    Gradient fg_grad;
    Gradient bg_grad;
    bool has_fg_grad = false;
    bool has_bg_grad = false;
    if (has_input_fg) {
        Color stops[2] = {cfg->correct_color, input_fg};
        if (gradient_with_steps(stops, 2, 10, false, &fg_grad) == 0) {
            has_fg_grad = true;
        }
    }
    if (has_input_bg) {
        Color stops[2] = {cfg->correct_color, input_bg};
        if (gradient_with_steps(stops, 2, 10, false, &bg_grad) == 0) {
            has_bg_grad = true;
        }
    }
    Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, final_scene);
    if (has_fg_grad || has_bg_grad) {
        const char *symbols[1] = {input_symbol};
        scene_apply_gradient_to_symbols(scene, symbols, 1, 3, has_fg_grad ? &fg_grad : NULL,
                                        has_bg_grad ? &bg_grad : NULL);
    } else {
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        scene_add_frame(scene, input_symbol, 3, &vp);
    }
    if (has_fg_grad) {
        gradient_free(&fg_grad);
    }
    if (has_bg_grad) {
        gradient_free(&bg_grad);
    }
    free(input_symbol);
    return final_scene;
}

// ErrorCorrectIterator._configure_swapped_character.
static int errorcorrect_configure_swapped(Effect *self, EngineCtx *ctx, CharId id, const Gradient *correcting_gradient) {
    ErrorCorrect *st = self->state;
    ErrorCorrectConfig *cfg = &st->config;
    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;

    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
    strcpy(input_symbol, ch->input_symbol);
    bool uses_pre = ch->uses_input_preexisting_colors;

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    const char *first_block_wipe =
        animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "", uses_pre);
    const char *last_block_wipe =
        animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "", uses_pre);

    ch = &ctx->terminal.arena.items[id];
    Scene *first_scene = (Scene *)om_get(&ch->animation.scenes, first_block_wipe);
    for (size_t i = 0; i < 8; i++) {
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors.has_fg = true;
        vp.colors.fg = cfg->error_color;
        if (scene_add_frame(first_scene, BLOCK_WIPE_START[i], 3, &vp) != 0) {
            free(input_symbol);
            return -1;
        }
    }

    ColorPair final_colors = st->final_colors[id];
    ch = &ctx->terminal.arena.items[id];
    Scene *last_scene = (Scene *)om_get(&ch->animation.scenes, last_block_wipe);
    if (dynamic) {
        for (size_t i = 0; i < 6; i++) {
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = cfg->correct_color;
            if (scene_add_frame(last_scene, BLOCK_WIPE_END[i], 3, &vp) != 0) {
                free(input_symbol);
                return -1;
            }
        }
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors = final_colors;
        if (scene_add_frame(last_scene, BLOCK_WIPE_END[6], 3, &vp) != 0) {
            free(input_symbol);
            return -1;
        }
    } else {
        for (size_t i = 0; i < 7; i++) {
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = cfg->correct_color;
            if (scene_add_frame(last_scene, BLOCK_WIPE_END[i], 3, &vp) != 0) {
                free(input_symbol);
                return -1;
            }
        }
    }

    ch = &ctx->terminal.arena.items[id];
    const char *initial_scene =
        animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "", uses_pre);
    Scene *initial = (Scene *)om_get(&ch->animation.scenes, initial_scene);
    {
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors.has_fg = true;
        vp.colors.fg = cfg->error_color;
        if (scene_add_frame(initial, input_symbol, 1, &vp) != 0) {
            free(input_symbol);
            return -1;
        }
    }
    engine_activate_scene(ctx, self, id, initial_scene);

    ch = &ctx->terminal.arena.items[id];
    const char *error_scene =
        animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "error", uses_pre);
    Scene *err = (Scene *)om_get(&ch->animation.scenes, error_scene);
    Color white;
    color_from_hex("ffffff", &white);
    for (int i = 0; i < 10; i++) {
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors.has_fg = true;
        vp.colors.fg = cfg->error_color;
        if (scene_add_frame(err, "▓", 3, &vp) != 0) {
            free(input_symbol);
            return -1;
        }
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors.has_fg = true;
        vp.colors.fg = white;
        if (scene_add_frame(err, input_symbol, 3, &vp) != 0) {
            free(input_symbol);
            return -1;
        }
    }

    ch = &ctx->terminal.arena.items[id];
    const char *correcting_scene =
        animation_new_scene(&ch->animation, false, true, SYNC_DISTANCE, false, no_ease, "", uses_pre);
    Scene *corr = (Scene *)om_get(&ch->animation.scenes, correcting_scene);
    {
        const char *symbols[1] = {"█"};
        if (scene_apply_gradient_to_symbols(corr, symbols, 1, 3, correcting_gradient, NULL) != 0) {
            free(input_symbol);
            return -1;
        }
    }

    const char *final_scene;
    if (dynamic) {
        final_scene = errorcorrect_dynamic_final_scene(ctx, cfg, id);
    } else {
        Color final_fg = st->final_colors[id].fg;
        Color stops[2] = {cfg->correct_color, final_fg};
        Gradient char_final_gradient;
        if (gradient_with_steps(stops, 2, 10, false, &char_final_gradient) != 0) {
            free(input_symbol);
            return -1;
        }
        ch = &ctx->terminal.arena.items[id];
        final_scene = animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "", uses_pre);
        Scene *scene = (Scene *)om_get(&ch->animation.scenes, final_scene);
        const char *symbols[1] = {input_symbol};
        int add_rc = scene_apply_gradient_to_symbols(scene, symbols, 1, 3, &char_final_gradient, NULL);
        gradient_free(&char_final_gradient);
        if (add_rc != 0) {
            free(input_symbol);
            return -1;
        }
    }

    int rc = 0;
    CallerKey caller;
    EventAction action;
    {
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_SCENE;
        caller.id = (char *)error_scene;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_SCENE;
        action.id = (char *)first_block_wipe;
        if (engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
            rc = -1;
        }
    }
    if (rc == 0) {
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_SCENE;
        caller.id = (char *)first_block_wipe;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_SCENE;
        action.id = (char *)correcting_scene;
        if (engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
            rc = -1;
        }
    }
    if (rc == 0) {
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_SCENE;
        caller.id = (char *)first_block_wipe;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_PATH;
        action.id = "input_coord";
        if (engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
            rc = -1;
        }
    }
    if (rc == 0) {
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = "input_coord";
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_SET_LAYER;
        action.layer = 1;
        if (engine_register_event(ctx, id, EVENT_PATH_ACTIVATED, &caller, &action) != 0) {
            rc = -1;
        }
    }
    if (rc == 0) {
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = "input_coord";
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_SET_LAYER;
        action.layer = 0;
        if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
            rc = -1;
        }
    }
    if (rc == 0) {
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = "input_coord";
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_SCENE;
        action.id = (char *)last_block_wipe;
        if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
            rc = -1;
        }
    }
    if (rc == 0) {
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_SCENE;
        caller.id = (char *)last_block_wipe;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_SCENE;
        action.id = (char *)final_scene;
        if (engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
            rc = -1;
        }
    }

    free(input_symbol);
    return rc;
}

static int errorcorrect_build(Effect *self, EngineCtx *ctx) {
    ErrorCorrect *st = self->state;
    ErrorCorrectConfig *cfg = &st->config;

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
    st->final_colors_len = arena_len;
    st->final_colors = calloc(arena_len ? arena_len : 1, sizeof(ColorPair));
    st->final_colors_present = calloc(arena_len ? arena_len : 1, sizeof(bool));

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    for (size_t i = 0; i < n; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        ColorPair cp;
        memset(&cp, 0, sizeof(cp));
        if (dynamic) {
            cp.has_fg = ch->animation.has_input_fg;
            cp.fg = ch->animation.input_fg_color;
            cp.has_bg = ch->animation.has_input_bg;
            cp.bg = ch->animation.input_bg_color;
        } else {
            const Color *mapped = coordcolormap_get(&mapping, ch->input_coord);
            cp.has_fg = true;
            if (mapped) {
                cp.fg = *mapped;
            }
        }
        if ((size_t)id < st->final_colors_len) {
            st->final_colors[id] = cp;
            st->final_colors_present[id] = true;
        }
    }

    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        ColorPair spawn_colors = st->final_colors[id];
        bool uses_pre = ch->uses_input_preexisting_colors;
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);

        const char *spawn_scene =
            animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "", uses_pre);
        Scene *scene = (Scene *)om_get(&ch->animation.scenes, spawn_scene);
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors = spawn_colors;
        if (scene_add_frame(scene, input_symbol, 1, &vp) != 0) {
            rc = -1;
        } else {
            engine_activate_scene(ctx, self, id, spawn_scene);
            terminal_set_character_visibility(&ctx->terminal, id, true);
        }
        free(input_symbol);
    }

    size_t all_len = ctx->terminal.input_characters_len;
    CharId *all_characters = malloc((all_len ? all_len : 1) * sizeof(CharId));
    memcpy(all_characters, ctx->terminal.input_characters, all_len * sizeof(CharId));

    Gradient correcting_gradient;
    if (rc == 0) {
        Color stops[2] = {cfg->error_color, cfg->correct_color};
        if (gradient_with_steps(stops, 2, 10, false, &correcting_gradient) != 0) {
            rc = -1;
        }
    }

    int64_t pair_count = (int64_t)(cfg->error_pairs * (double)n);
    for (int64_t pi = 0; pi < pair_count && rc == 0; pi++) {
        if (all_len < 2) {
            break;
        }
        int64_t index1 = rng_randrange(&ctx->rng, 0, (int64_t)all_len);
        CharId char1 = all_characters[index1];
        memmove(&all_characters[index1], &all_characters[index1 + 1],
                (all_len - (size_t)index1 - 1) * sizeof(CharId));
        all_len--;
        int64_t index2 = rng_randrange(&ctx->rng, 0, (int64_t)all_len);
        CharId char2 = all_characters[index2];
        memmove(&all_characters[index2], &all_characters[index2 + 1],
                (all_len - (size_t)index2 - 1) * sizeof(CharId));
        all_len--;

        Coord char1_input = ctx->terminal.arena.items[char1].input_coord;
        Coord char2_input = ctx->terminal.arena.items[char2].input_coord;

        motion_set_coordinate(&ctx->terminal.arena.items[char1].motion, char2_input);
        char *path1 = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[char1].motion, cfg->movement_speed, false, no_ease, false, 0,
                            0, false, "input_coord", &path1) != 0) {
            rc = -1;
            break;
        }
        Path *p1 = (Path *)om_get(&ctx->terminal.arena.items[char1].motion.paths, path1);
        Waypoint wp;
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(p1, char1_input, NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(path1);
            rc = -1;
            break;
        }
        waypoint_free(&wp);
        free(path1);

        motion_set_coordinate(&ctx->terminal.arena.items[char2].motion, char1_input);
        char *path2 = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[char2].motion, cfg->movement_speed, false, no_ease, false, 0,
                            0, false, "input_coord", &path2) != 0) {
            rc = -1;
            break;
        }
        Path *p2 = (Path *)om_get(&ctx->terminal.arena.items[char2].motion.paths, path2);
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(p2, char2_input, NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(path2);
            rc = -1;
            break;
        }
        waypoint_free(&wp);
        free(path2);

        if (st->swapped_len == 0 || st->swapped_len % 8 == 0) {
            size_t cap = st->swapped_len + 8;
            SwapPair *grown = realloc(st->swapped, cap * sizeof(SwapPair));
            if (!grown) {
                rc = -1;
                break;
            }
            st->swapped = grown;
        }
        st->swapped[st->swapped_len].a = char1;
        st->swapped[st->swapped_len].b = char2;
        st->swapped_len++;

        if (errorcorrect_configure_swapped(self, ctx, char1, &correcting_gradient) != 0 ||
            errorcorrect_configure_swapped(self, ctx, char2, &correcting_gradient) != 0) {
            rc = -1;
        }
    }

    if (rc == 0) {
        free(correcting_gradient.spectrum);
    }

    free(all_characters);
    free(characters);
    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    st->swap_delay = 0;
    return rc;
}

static const char *errorcorrect_next_frame(Effect *self, EngineCtx *ctx) {
    ErrorCorrect *st = self->state;
    if (st->swapped_len > 0 && st->swap_delay == 0) {
        SwapPair pair = st->swapped[0];
        memmove(&st->swapped[0], &st->swapped[1], (st->swapped_len - 1) * sizeof(SwapPair));
        st->swapped_len--;
        CharId ids[2] = {pair.a, pair.b};
        for (int k = 0; k < 2; k++) {
            engine_activate_scene(ctx, self, ids[k], "error");
            ac_insert(&ctx->active_characters, ids[k]);
        }
        st->swap_delay = st->config.swap_delay;
    } else if (st->swap_delay != 0) {
        st->swap_delay -= 1;
    }
    if (!ac_is_empty(&ctx->active_characters)) {
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void errorcorrect_destroy(Effect *self) {
    ErrorCorrect *st = self->state;
    if (!st) {
        return;
    }
    free(st->swapped);
    free(st->final_colors);
    free(st->final_colors_present);
    free(st);
    free(self);
}

static const EffectOps ERRORCORRECT_OPS = {errorcorrect_build, errorcorrect_next_frame, errorcorrect_destroy, NULL};

Effect *errorcorrect_make(const void *cfg) {
    ErrorCorrect *st = calloc(1, sizeof(ErrorCorrect));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const ErrorCorrectConfig *)cfg;
    effect->ops = &ERRORCORRECT_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec errorcorrect_specs[] = {
    EF_SPEC("error-pairs", 0, EF_FLOAT_POS, offsetof(ErrorCorrectConfig, error_pairs)),
    EF_SPEC("swap-delay", 0, EF_POS_INT, offsetof(ErrorCorrectConfig, swap_delay)),
    EF_SPEC("error-color", 0, EF_COLOR, offsetof(ErrorCorrectConfig, error_color)),
    EF_SPEC("correct-color", 0, EF_COLOR, offsetof(ErrorCorrectConfig, correct_color)),
    EF_SPEC("movement-speed", 0, EF_FLOAT_POS, offsetof(ErrorCorrectConfig, movement_speed)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(ErrorCorrectConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(ErrorCorrectConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(ErrorCorrectConfig, final_gradient_direction)),
};

const EffectEntry errorcorrect_entry = {
    "errorcorrect",
    errorcorrect_specs,
    sizeof(errorcorrect_specs) / sizeof(errorcorrect_specs[0]),
    sizeof(ErrorCorrectConfig),
    errorcorrect_config_defaults,
    errorcorrect_free_config,
    errorcorrect_make,
};
