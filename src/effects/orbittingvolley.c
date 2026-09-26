#include "effects/orbittingvolley.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/events.h"
#include "engine/motion.h"
#include "engine/terminal.h"
#include "utils/easing.h"
#include "utils/geometry.h"
#include "utils/graphics.h"
#include "utils/ordmap.h"

typedef struct {
    CharId *items;
    size_t len;
    size_t cap;
} OvIdVec;

typedef struct {
    CharId character;
    OvIdVec magazine;
} OvLauncher;

typedef struct {
    OrbittingVolleyConfig config;
    ColorPair *final_colors;
    bool *final_present;
    size_t map_len;
    CoordColorMap launcher_gradient_coordinate_map;
    bool has_last_color;
    Color last_color;
    OvLauncher *launchers;
    size_t launchers_len;
    size_t launchers_cap;
    int64_t delay;
    bool complete;
} OrbittingVolley;

static char *ov_dup(const char *s) {
    char *copy = malloc(strlen(s) + 1);
    if (copy) {
        strcpy(copy, s);
    }
    return copy;
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

static void ov_idvec_push(OvIdVec *v, CharId id) {
    if (v->len == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 16;
        CharId *grown = realloc(v->items, cap * sizeof(CharId));
        if (!grown) {
            return;
        }
        v->items = grown;
        v->cap = cap;
    }
    v->items[v->len++] = id;
}

static CharId ov_idvec_remove0(OvIdVec *v) {
    CharId value = v->items[0];
    for (size_t i = 1; i < v->len; i++) {
        v->items[i - 1] = v->items[i];
    }
    v->len -= 1;
    return value;
}

static void ov_idvec_free(OvIdVec *v) {
    free(v->items);
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

static void ov_launchers_push(OrbittingVolley *st, OvLauncher launcher) {
    if (st->launchers_len == st->launchers_cap) {
        size_t cap = st->launchers_cap ? st->launchers_cap * 2 : 4;
        OvLauncher *grown = realloc(st->launchers, cap * sizeof(OvLauncher));
        if (!grown) {
            return;
        }
        st->launchers = grown;
        st->launchers_cap = cap;
    }
    st->launchers[st->launchers_len++] = launcher;
}

void orbittingvolley_config_defaults(void *cfg_ptr) {
    OrbittingVolleyConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->top_launcher_symbol = ov_dup("\xe2\x96\x88");
    cfg->right_launcher_symbol = ov_dup("\xe2\x96\x88");
    cfg->bottom_launcher_symbol = ov_dup("\xe2\x96\x88");
    cfg->left_launcher_symbol = ov_dup("\xe2\x96\x88");
    cfg->launcher_movement_speed = 0.8;
    cfg->character_movement_speed = 1.5;
    cfg->volley_size = 0.03;
    cfg->launch_delay = 30;
    easing_parse("out_sine", &cfg->character_easing);
    push_color_default(&cfg->final_gradient_stops, "FFA15C");
    push_color_default(&cfg->final_gradient_stops, "44D492");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_RADIAL;
}

void orbittingvolley_free_config(void *cfg_ptr) {
    OrbittingVolleyConfig *cfg = cfg_ptr;
    free(cfg->top_launcher_symbol);
    free(cfg->right_launcher_symbol);
    free(cfg->bottom_launcher_symbol);
    free(cfg->left_launcher_symbol);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

// Launcher.build_paths (only called for the main launcher).
static int ov_build_launcher_paths(OrbittingVolley *st, EngineCtx *ctx, CharId id) {
    Coord waypoints[2];
    waypoints[0] = coord_new(ctx->terminal.canvas.left, ctx->terminal.canvas.top);
    waypoints[1] = coord_new(ctx->terminal.canvas.right, ctx->terminal.canvas.top);
    Coord input_coord = ctx->terminal.arena.items[id].input_coord;
    int waypoint_start_index = -1;
    for (int i = 0; i < 2; i++) {
        if (coord_eq(waypoints[i], input_coord)) {
            waypoint_start_index = i;
            break;
        }
    }
    if (waypoint_start_index < 0) {
        return -1;
    }
    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));
    char *perimeter_id = NULL;
    if (motion_new_path(&ctx->terminal.arena.items[id].motion, st->config.launcher_movement_speed, false, no_ease,
                        true, 2, 0, false, "perimeter", &perimeter_id) != 0) {
        return -1;
    }
    Path *path = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, "perimeter");
    for (int k = 0; k < 2; k++) {
        int index = (waypoint_start_index + k) % 2;
        Waypoint wp;
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(path, waypoints[index], NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(perimeter_id);
            return -1;
        }
        waypoint_free(&wp);
    }
    free(perimeter_id);
    return 0;
}

// Launcher.launch.
static bool ov_launch(OrbittingVolley *st, EngineCtx *ctx, Effect *self, size_t launcher_index, CharId *out) {
    OvLauncher *launcher = &st->launchers[launcher_index];
    if (launcher->magazine.len == 0) {
        return false;
    }
    CharId next_char = ov_idvec_remove0(&launcher->magazine);
    Coord launcher_coord = ctx->terminal.arena.items[launcher->character].motion.current_coord;
    motion_set_coordinate(&ctx->terminal.arena.items[next_char].motion, launcher_coord);
    engine_activate_path(ctx, self, next_char, "input_path");
    terminal_set_character_visibility(&ctx->terminal, next_char, true);
    *out = next_char;
    return true;
}

// OrbittingVolleyIterator._set_launcher_coordinates.
static void ov_set_launcher_coordinates(OrbittingVolley *st, EngineCtx *ctx, size_t parent_index, size_t child_index) {
    int64_t canvas_top = ctx->terminal.canvas.top;
    int64_t canvas_bottom = ctx->terminal.canvas.bottom;
    int64_t canvas_left = ctx->terminal.canvas.left;
    int64_t canvas_right = ctx->terminal.canvas.right;
    CharId parent_char = st->launchers[parent_index].character;
    CharId child_char = st->launchers[child_index].character;
    double parent_progress =
        (double)ctx->terminal.arena.items[parent_char].motion.current_coord.column / (double)canvas_right;
    Coord child_input_coord = ctx->terminal.arena.items[child_char].input_coord;
    if (coord_eq(child_input_coord, coord_new(canvas_right, canvas_top))) {
        int64_t child_row = canvas_top - (int64_t)((double)canvas_top * parent_progress);
        motion_set_coordinate(&ctx->terminal.arena.items[child_char].motion,
                              coord_new(canvas_right, child_row > 1 ? child_row : 1));
    } else if (coord_eq(child_input_coord, coord_new(canvas_right, canvas_bottom))) {
        int64_t child_column = canvas_right - (int64_t)((double)canvas_right * parent_progress);
        motion_set_coordinate(&ctx->terminal.arena.items[child_char].motion,
                              coord_new(child_column > 1 ? child_column : 1, canvas_bottom));
    } else if (coord_eq(child_input_coord, coord_new(canvas_left, canvas_bottom))) {
        int64_t child_row = canvas_bottom + (int64_t)((double)canvas_top * parent_progress);
        motion_set_coordinate(&ctx->terminal.arena.items[child_char].motion,
                              coord_new(canvas_left, child_row < canvas_top ? child_row : canvas_top));
    }
    Coord current_coord = ctx->terminal.arena.items[child_char].motion.current_coord;
    const Color *mapped = coordcolormap_get(&st->launcher_gradient_coordinate_map, current_coord);
    Color color;
    memset(&color, 0, sizeof(color));
    if (mapped) {
        color = *mapped;
    }
    EffectCharacter *ch = &ctx->terminal.arena.items[child_char];
    char *input_symbol = ov_dup(ch->input_symbol);
    bool uses_pre = ch->uses_input_preexisting_colors;
    ColorPair colors;
    memset(&colors, 0, sizeof(colors));
    colors.has_fg = true;
    colors.fg = color;
    animation_set_appearance(&ctx->terminal.arena.items[child_char].animation, uses_pre, input_symbol, &colors);
    free(input_symbol);
}

static int orbittingvolley_build(Effect *self, EngineCtx *ctx) {
    OrbittingVolley *st = self->state;
    OrbittingVolleyConfig *cfg = &st->config;
    int rc = 0;

    Gradient final_gradient;
    memset(&final_gradient, 0, sizeof(final_gradient));
    if (gradient_new(cfg->final_gradient_stops.items, cfg->final_gradient_stops.len,
                     cfg->final_gradient_steps.items, cfg->final_gradient_steps.len, false, false,
                     &final_gradient) != 0) {
        return -1;
    }
    CoordColorMap final_gradient_coordinate_map;
    memset(&final_gradient_coordinate_map, 0, sizeof(final_gradient_coordinate_map));
    if (gradient_build_coordinate_color_mapping(&final_gradient, ctx->terminal.canvas.text_bottom,
                                                ctx->terminal.canvas.text_top, ctx->terminal.canvas.text_left,
                                                ctx->terminal.canvas.text_right, cfg->final_gradient_direction,
                                                &final_gradient_coordinate_map) != 0) {
        gradient_free(&final_gradient);
        return -1;
    }
    if (gradient_build_coordinate_color_mapping(&final_gradient, ctx->terminal.canvas.bottom,
                                                ctx->terminal.canvas.top, ctx->terminal.canvas.left,
                                                ctx->terminal.canvas.right, cfg->final_gradient_direction,
                                                &st->launcher_gradient_coordinate_map) != 0) {
        coordcolormap_free(&final_gradient_coordinate_map);
        gradient_free(&final_gradient);
        return -1;
    }
    st->has_last_color = final_gradient.len > 0;
    if (st->has_last_color) {
        st->last_color = final_gradient.spectrum[final_gradient.len - 1];
    }

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    CharacterFilter filter = character_filter_default();
    size_t characters_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, filter,
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &characters_len);

    st->map_len = ctx->terminal.arena.len;
    st->final_colors = calloc(st->map_len ? st->map_len : 1, sizeof(ColorPair));
    st->final_present = calloc(st->map_len ? st->map_len : 1, sizeof(bool));

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    for (size_t i = 0; i < characters_len && rc == 0; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        Coord input_coord = ch->input_coord;
        char *input_symbol = ov_dup(ch->input_symbol);
        bool uses_pre = ch->uses_input_preexisting_colors;
        bool has_input_fg = ch->animation.has_input_fg;
        Color input_fg = ch->animation.input_fg_color;
        bool has_input_bg = ch->animation.has_input_bg;
        Color input_bg = ch->animation.input_bg_color;
        ColorPair final_colors;
        memset(&final_colors, 0, sizeof(final_colors));
        if (dynamic) {
            final_colors.has_fg = has_input_fg;
            final_colors.fg = input_fg;
            final_colors.has_bg = has_input_bg;
            final_colors.bg = input_bg;
        } else {
            const Color *mapped = coordcolormap_get(&final_gradient_coordinate_map, input_coord);
            final_colors.has_fg = true;
            if (mapped) {
                final_colors.fg = *mapped;
            }
        }
        if ((size_t)id < st->map_len) {
            st->final_colors[id] = final_colors;
            st->final_present[id] = true;
        }
        {
            char *input_path = NULL;
            if (motion_new_path(&ctx->terminal.arena.items[id].motion, cfg->character_movement_speed, true,
                                cfg->character_easing, true, 1, 0, false, "input_path", &input_path) != 0) {
                rc = -1;
            } else {
                Path *path = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, input_path);
                Waypoint wp;
                memset(&wp, 0, sizeof(wp));
                if (path_new_waypoint(path, input_coord, NULL, 0, "", &wp) != 0) {
                    waypoint_free(&wp);
                    rc = -1;
                }
                waypoint_free(&wp);
                free(input_path);
            }
        }
        if (rc == 0) {
            CallerKey caller;
            memset(&caller, 0, sizeof(caller));
            caller.kind = CALLER_PATH;
            caller.id = "input_path";
            EventAction action;
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_SET_LAYER;
            action.layer = 0;
            if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
                rc = -1;
            }
        }
        if (rc == 0) {
            animation_set_appearance(&ctx->terminal.arena.items[id].animation, uses_pre, input_symbol,
                                     &final_colors);
        }
        free(input_symbol);
    }
    free(characters);

    if (rc == 0) {
        const char *symbols[4] = {cfg->top_launcher_symbol, cfg->right_launcher_symbol,
                                  cfg->bottom_launcher_symbol, cfg->left_launcher_symbol};
        Coord coords[4];
        coords[0] = coord_new(ctx->terminal.canvas.left, ctx->terminal.canvas.top);
        coords[1] = coord_new(ctx->terminal.canvas.right, ctx->terminal.canvas.top);
        coords[2] = coord_new(ctx->terminal.canvas.right, ctx->terminal.canvas.bottom);
        coords[3] = coord_new(ctx->terminal.canvas.left, ctx->terminal.canvas.bottom);
        for (int i = 0; i < 4 && rc == 0; i++) {
            CharId character = terminal_add_character(&ctx->terminal, symbols[i], coords[i]);
            ctx->terminal.arena.items[character].layer = 2;
            terminal_set_character_visibility(&ctx->terminal, character, true);
            ac_insert(&ctx->active_characters, character);
            OvLauncher launcher;
            memset(&launcher, 0, sizeof(launcher));
            launcher.character = character;
            ov_launchers_push(st, launcher);
        }
    }

    if (rc == 0) {
        CharId main_character = st->launchers[0].character;
        EffectCharacter *ch = &ctx->terminal.arena.items[main_character];
        char *input_symbol = ov_dup(ch->input_symbol);
        bool uses_pre = ch->uses_input_preexisting_colors;
        ColorPair colors;
        memset(&colors, 0, sizeof(colors));
        colors.has_fg = st->has_last_color;
        if (st->has_last_color) {
            colors.fg = st->last_color;
        }
        animation_set_appearance(&ctx->terminal.arena.items[main_character].animation, uses_pre, input_symbol,
                                 &colors);
        free(input_symbol);
        if (ov_build_launcher_paths(st, ctx, main_character) != 0) {
            rc = -1;
        } else {
            engine_activate_path(ctx, self, main_character, "perimeter");
        }
    }

    if (rc == 0) {
        CharIdGrouping grouped =
            terminal_get_characters_grouped(&ctx->terminal, filter, CG_CENTER_TO_OUTSIDE);
        size_t index = 0;
        for (size_t gi = 0; gi < grouped.len; gi++) {
            for (size_t ci = 0; ci < grouped.buckets[gi].len; ci++) {
                CharId character = grouped.buckets[gi].items[ci];
                size_t launcher_index = index % st->launchers_len;
                ov_idvec_push(&st->launchers[launcher_index].magazine, character);
                index++;
            }
        }
        charidgrouping_free(&grouped);
        st->delay = 0;
    }

    coordcolormap_free(&final_gradient_coordinate_map);
    gradient_free(&final_gradient);
    return rc;
}

static const char *orbittingvolley_next_frame(Effect *self, EngineCtx *ctx) {
    OrbittingVolley *st = self->state;
    OrbittingVolleyConfig *cfg = &st->config;
    bool any_magazine = false;
    for (size_t i = 0; i < st->launchers_len; i++) {
        if (st->launchers[i].magazine.len > 0) {
            any_magazine = true;
            break;
        }
    }
    if (any_magazine || ac_len(&ctx->active_characters) > 1) {
        CharId main_character = st->launchers[0].character;
        if (ctx->terminal.arena.items[main_character].motion.active_path == NULL) {
            Path *perimeter = (Path *)om_get(&ctx->terminal.arena.items[main_character].motion.paths, "perimeter");
            if (perimeter && perimeter->waypoints_len > 0) {
                Coord first_waypoint_coord = perimeter->waypoints[0].coord;
                motion_set_coordinate(&ctx->terminal.arena.items[main_character].motion, first_waypoint_coord);
            }
            engine_activate_path(ctx, self, main_character, "perimeter");
            ac_insert(&ctx->active_characters, main_character);
        }
        {
            Coord current_coord = ctx->terminal.arena.items[main_character].motion.current_coord;
            const Color *mapped = coordcolormap_get(&st->launcher_gradient_coordinate_map, current_coord);
            Color color;
            memset(&color, 0, sizeof(color));
            if (mapped) {
                color = *mapped;
            }
            EffectCharacter *ch = &ctx->terminal.arena.items[main_character];
            bool uses_pre = ch->uses_input_preexisting_colors;
            ColorPair colors;
            memset(&colors, 0, sizeof(colors));
            colors.has_fg = true;
            colors.fg = color;
            animation_set_appearance(&ctx->terminal.arena.items[main_character].animation, uses_pre,
                                     cfg->top_launcher_symbol, &colors);
        }
        for (size_t child_index = 1; child_index < st->launchers_len; child_index++) {
            ov_set_launcher_coordinates(st, ctx, 0, child_index);
        }
        if (st->delay == 0) {
            for (size_t launcher_index = 0; launcher_index < st->launchers_len; launcher_index++) {
                int64_t characters_to_launch =
                    (int64_t)((cfg->volley_size * (double)ctx->terminal.input_characters_len) / 4.0);
                if (characters_to_launch < 1) {
                    characters_to_launch = 1;
                }
                for (int64_t k = 0; k < characters_to_launch; k++) {
                    CharId next_char;
                    if (ov_launch(st, ctx, self, launcher_index, &next_char)) {
                        ac_insert(&ctx->active_characters, next_char);
                    }
                }
            }
            st->delay = cfg->launch_delay;
        } else {
            st->delay -= 1;
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    if (!st->complete) {
        st->complete = true;
        for (size_t launcher_index = 0; launcher_index < st->launchers_len; launcher_index++) {
            terminal_set_character_visibility(&ctx->terminal, st->launchers[launcher_index].character, false);
        }
        return engine_frame(ctx);
    }
    return NULL;
}

static void orbittingvolley_destroy(Effect *self) {
    OrbittingVolley *st = self->state;
    if (!st) {
        return;
    }
    for (size_t i = 0; i < st->launchers_len; i++) {
        ov_idvec_free(&st->launchers[i].magazine);
    }
    free(st->launchers);
    free(st->final_colors);
    free(st->final_present);
    coordcolormap_free(&st->launcher_gradient_coordinate_map);
    free(st);
    free(self);
}

static const EffectOps ORBITTINGVOLLEY_OPS = {orbittingvolley_build, orbittingvolley_next_frame,
                                              orbittingvolley_destroy, NULL};

Effect *orbittingvolley_make(const void *cfg) {
    OrbittingVolley *st = calloc(1, sizeof(OrbittingVolley));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const OrbittingVolleyConfig *)cfg;
    effect->ops = &ORBITTINGVOLLEY_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec orbittingvolley_specs[] = {
    EF_SPEC("top-launcher-symbol", 0, EF_SYMBOL, offsetof(OrbittingVolleyConfig, top_launcher_symbol)),
    EF_SPEC("right-launcher-symbol", 0, EF_SYMBOL, offsetof(OrbittingVolleyConfig, right_launcher_symbol)),
    EF_SPEC("bottom-launcher-symbol", 0, EF_SYMBOL, offsetof(OrbittingVolleyConfig, bottom_launcher_symbol)),
    EF_SPEC("left-launcher-symbol", 0, EF_SYMBOL, offsetof(OrbittingVolleyConfig, left_launcher_symbol)),
    EF_SPEC("launcher-movement-speed", 0, EF_FLOAT_POS, offsetof(OrbittingVolleyConfig, launcher_movement_speed)),
    EF_SPEC("character-movement-speed", 0, EF_FLOAT_POS, offsetof(OrbittingVolleyConfig, character_movement_speed)),
    EF_SPEC("volley-size", 0, EF_RATIO_NONNEG, offsetof(OrbittingVolleyConfig, volley_size)),
    EF_SPEC("launch-delay", 0, EF_NONNEG_INT, offsetof(OrbittingVolleyConfig, launch_delay)),
    EF_SPEC("character-easing", 0, EF_EASING, offsetof(OrbittingVolleyConfig, character_easing)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(OrbittingVolleyConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(OrbittingVolleyConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(OrbittingVolleyConfig, final_gradient_direction)),
};

const EffectEntry orbittingvolley_entry = {
    "orbittingvolley",
    orbittingvolley_specs,
    sizeof(orbittingvolley_specs) / sizeof(orbittingvolley_specs[0]),
    sizeof(OrbittingVolleyConfig),
    orbittingvolley_config_defaults,
    orbittingvolley_free_config,
    orbittingvolley_make,
};
