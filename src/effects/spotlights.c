#include "effects/spotlights.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/active_characters.h"
#include "engine/animation.h"
#include "engine/canvas.h"
#include "engine/character.h"
#include "engine/ctx.h"
#include "engine/motion.h"
#include "engine/terminal.h"
#include "utils/easing.h"
#include "utils/geometry.h"
#include "utils/graphics.h"
#include "utils/pycompat.h"

typedef struct {
    ColorPair bright;
    ColorPair dark;
} ColorPair2;

typedef struct {
    SpotlightsConfig config;
    ActiveCharacters illuminated_chars;
    ActiveCharacters illuminated_scratch;
    ColorPair2 *color_map;
    bool *color_present;
    size_t map_len;
    CharId *spotlights;
    size_t spotlights_len;
    size_t spotlights_cap;
    int64_t illuminate_range;
    int64_t search_duration;
    bool searching;
    bool expanding;
    bool complete;
} Spotlights;

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

static void spotlight_push(Spotlights *st, CharId id) {
    if (st->spotlights_len == st->spotlights_cap) {
        size_t cap = st->spotlights_cap ? st->spotlights_cap * 2 : 8;
        CharId *grown = realloc(st->spotlights, cap * sizeof(CharId));
        if (!grown) {
            return;
        }
        st->spotlights = grown;
        st->spotlights_cap = cap;
    }
    st->spotlights[st->spotlights_len++] = id;
}

void spotlights_config_defaults(void *cfg_ptr) {
    SpotlightsConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->beam_width_ratio = 2.0;
    cfg->beam_falloff = 0.3;
    cfg->search_duration = 550;
    cfg->search_speed_range.start = 0.35;
    cfg->search_speed_range.end = 0.75;
    cfg->spotlight_count = 3;
    push_color_default(&cfg->final_gradient_stops, "ab48ff");
    push_color_default(&cfg->final_gradient_stops, "e7b2b2");
    push_color_default(&cfg->final_gradient_stops, "fffebd");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void spotlights_free_config(void *cfg_ptr) {
    SpotlightsConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static ColorPair adjust_pair(const ColorPair *in, double factor) {
    ColorPair out;
    memset(&out, 0, sizeof(out));
    out.has_fg = in->has_fg;
    if (in->has_fg) {
        out.fg = color_adjust_brightness(&in->fg, factor);
    }
    out.has_bg = in->has_bg;
    if (in->has_bg) {
        out.bg = color_adjust_brightness(&in->bg, factor);
    }
    return out;
}

static bool has_input_colors(const EngineCtx *ctx, CharId id) {
    const EffectCharacter *ch = &ctx->terminal.arena.items[id];
    return ch->animation.has_input_fg || ch->animation.has_input_bg;
}

static bool is_spotlightable(const EngineCtx *ctx, CharId id) {
    const EffectCharacter *ch = &ctx->terminal.arena.items[id];
    return strcmp(ch->input_symbol, " ") != 0 || has_input_colors(ctx, id);
}

// Returns true and fills *out when an expand override applies.
static bool get_expand_color_override(const Spotlights *st, const EngineCtx *ctx, CharId id, ColorPair *out) {
    if (ctx->terminal.config.existing_color_handling != EXISTING_COLOR_DYNAMIC || !st->expanding) {
        return false;
    }
    const EffectCharacter *ch = &ctx->terminal.arena.items[id];
    if (!ch->animation.has_input_fg && ch->animation.has_input_bg) {
        memset(out, 0, sizeof(*out));
        out->has_bg = true;
        out->bg = ch->animation.input_bg_color;
        return true;
    }
    if (!has_input_colors(ctx, id)) {
        memset(out, 0, sizeof(*out));
        return true;
    }
    return false;
}

// Reference Canvas.random_coord(rng, false, false) evaluates its column and row
// arguments left-to-right. Calling the engine helper here would let the C
// compiler evaluate them right-to-left and swap the two RNG draws, so sequence
// them explicitly to preserve the reference RNG order.
static Coord canvas_random_coord_inside(EngineCtx *ctx) {
    int64_t column = canvas_random_column(&ctx->terminal.canvas, &ctx->rng, false);
    int64_t row = canvas_random_row(&ctx->terminal.canvas, &ctx->rng, false);
    return coord_new(column, row);
}

static Coord find_coord_at_minimum_distance(EngineCtx *ctx, Coord origin_coord, int64_t minimum_distance) {
    for (;;) {
        Coord coord = canvas_random_coord_inside(ctx);
        double distance = find_length_of_line(origin_coord, coord, false);
        if (distance >= (double)minimum_distance) {
            return coord;
        }
    }
}

static int make_spotlights(Spotlights *st, EngineCtx *ctx, int64_t num) {
    int64_t minimum_distance = py_floor_div(ctx->terminal.canvas.right, 4);
    for (int64_t s = 0; s < num; s++) {
        Coord spawn_coord = canvas_random_coord(&ctx->terminal.canvas, &ctx->rng, true, false);
        CharId spotlight = terminal_add_character(&ctx->terminal, "O", spawn_coord);
        spotlight_push(st, spotlight);

        CoordVec targets;
        coordvec_init(&targets);
        Coord last_coord = canvas_random_coord_inside(ctx);
        coordvec_push(&targets, last_coord);
        for (int i = 0; i < 10; i++) {
            last_coord = find_coord_at_minimum_distance(ctx, last_coord, minimum_distance);
            coordvec_push(&targets, last_coord);
        }
        char **paths = NULL;
        size_t paths_len = 0;
        size_t paths_cap = 0;
        for (size_t i = 0; i < targets.len; i++) {
            double speed = rng_uniform(&ctx->rng, st->config.search_speed_range.start,
                                       st->config.search_speed_range.end);
            char path_id[32];
            snprintf(path_id, sizeof(path_id), "%zu", paths_len);
            char *pid = NULL;
            if (motion_new_path(&ctx->terminal.arena.items[spotlight].motion, speed, true,
                                easing_named(EASE_IN_OUT_QUAD), false, 0, 0, false, path_id, &pid) != 0) {
                coordvec_free(&targets);
                return -1;
            }
            Coord bezier_control = canvas_random_coord(&ctx->terminal.canvas, &ctx->rng, true, false);
            Path *p = (Path *)om_get(&ctx->terminal.arena.items[spotlight].motion.paths, pid);
            Waypoint wp;
            memset(&wp, 0, sizeof(wp));
            if (path_new_waypoint(p, targets.items[i], &bezier_control, 1, "", &wp) != 0) {
                waypoint_free(&wp);
                free(pid);
                for (size_t j = 0; j < paths_len; j++) {
                    free(paths[j]);
                }
                free(paths);
                coordvec_free(&targets);
                return -1;
            }
            waypoint_free(&wp);
            if (paths_len == paths_cap) {
                size_t cap = paths_cap ? paths_cap * 2 : 16;
                char **grown = realloc(paths, cap * sizeof(char *));
                if (!grown) {
                    free(pid);
                    coordvec_free(&targets);
                    return -1;
                }
                paths = grown;
                paths_cap = cap;
            }
            paths[paths_len++] = pid;
        }
        if (engine_chain_paths(ctx, spotlight, (const char *const *)paths, paths_len, true) != 0) {
            for (size_t j = 0; j < paths_len; j++) {
                free(paths[j]);
            }
            free(paths);
            coordvec_free(&targets);
            return -1;
        }
        for (size_t j = 0; j < paths_len; j++) {
            free(paths[j]);
        }
        free(paths);
        coordvec_free(&targets);

        Coord canvas_center = ctx->terminal.canvas.center;
        char *center_path = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[spotlight].motion, 0.5, true,
                            easing_named(EASE_IN_OUT_SINE), false, 0, 0, false, "center", &center_path) != 0) {
            return -1;
        }
        Path *cp = (Path *)om_get(&ctx->terminal.arena.items[spotlight].motion.paths, center_path);
        Waypoint wp;
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(cp, canvas_center, NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(center_path);
            return -1;
        }
        waypoint_free(&wp);
        free(center_path);
    }
    return 0;
}

static void illuminate_chars(Spotlights *st, EngineCtx *ctx, int64_t range_) {
    ActiveCharacters chars_in_range = st->illuminated_scratch;
    memset(&st->illuminated_scratch, 0, sizeof(st->illuminated_scratch));
    ac_clear(&chars_in_range);

    for (size_t i = 0; i < st->spotlights_len; i++) {
        CharId spotlight = st->spotlights[i];
        Coord current_coord = ctx->terminal.arena.items[spotlight].motion.current_coord;
        CoordVec circle = find_coords_in_circle(current_coord, range_);
        for (size_t j = 0; j < circle.len; j++) {
            CharId id = terminal_get_character_by_input_coord(&ctx->terminal, circle.items[j]);
            if (id == CHAR_ID_NONE) {
                continue;
            }
            if (is_spotlightable(ctx, id)) {
                ac_insert(&chars_in_range, id);
            }
        }
        coordvec_free(&circle);
    }

    for (size_t i = 0; i < st->illuminated_chars.len; i++) {
        CharId id = st->illuminated_chars.items[i];
        if (ac_contains(&chars_in_range, id)) {
            continue;
        }
        ColorPair colors;
        if (!get_expand_color_override(st, ctx, id, &colors)) {
            colors = st->color_map[id].dark;
        }
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        bool uses_pre = ch->uses_input_preexisting_colors;
        animation_set_appearance(&ch->animation, uses_pre, ch->input_symbol, &colors);
    }

    for (size_t i = 0; i < chars_in_range.len; i++) {
        CharId id = chars_in_range.items[i];
        Coord input_coord = ctx->terminal.arena.items[id].input_coord;
        double distance = INFINITY;
        for (size_t j = 0; j < st->spotlights_len; j++) {
            CharId spotlight = st->spotlights[j];
            Coord current_coord = ctx->terminal.arena.items[spotlight].motion.current_coord;
            double d = find_length_of_line(current_coord, input_coord, true);
            if (d < distance) {
                distance = d;
            }
        }
        ColorPair adjusted;
        if (distance > (double)range_ * (1.0 - st->config.beam_falloff)) {
            double brightness_factor =
                1.0 - (distance - (double)range_ * (1.0 - st->config.beam_falloff)) /
                          ((double)range_ * st->config.beam_falloff);
            if (brightness_factor < 0.2) {
                brightness_factor = 0.2;
            }
            adjusted = adjust_pair(&st->color_map[id].bright, brightness_factor);
        } else {
            adjusted = st->color_map[id].bright;
        }
        ColorPair colors;
        if (!get_expand_color_override(st, ctx, id, &colors)) {
            colors = adjusted;
        }
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        bool uses_pre = ch->uses_input_preexisting_colors;
        animation_set_appearance(&ch->animation, uses_pre, ch->input_symbol, &colors);
    }

    ActiveCharacters old = st->illuminated_chars;
    st->illuminated_chars = chars_in_range;
    st->illuminated_scratch = old;
}

static int spotlights_build(Effect *self, EngineCtx *ctx) {
    Spotlights *st = self->state;
    SpotlightsConfig *cfg = &st->config;

    Color dynamic_neutral_gray;
    color_from_hex("#808080", &dynamic_neutral_gray);

    if (make_spotlights(st, ctx, cfg->spotlight_count) != 0) {
        return -1;
    }

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
    st->map_len = arena_len;
    st->color_map = calloc(arena_len ? arena_len : 1, sizeof(ColorPair2));
    st->color_present = calloc(arena_len ? arena_len : 1, sizeof(bool));

    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        Coord input_coord = ch->input_coord;
        bool has_input_fg = ch->animation.has_input_fg;
        Color input_fg = ch->animation.input_fg_color;
        bool has_input_bg = ch->animation.has_input_bg;
        Color input_bg = ch->animation.input_bg_color;
        bool uses_pre = ch->uses_input_preexisting_colors;
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);

        ColorPair bright_pair;
        ColorPair dark_pair;
        memset(&bright_pair, 0, sizeof(bright_pair));
        memset(&dark_pair, 0, sizeof(dark_pair));
        if (dynamic) {
            if (has_input_fg || has_input_bg) {
                bool bright_has_fg = has_input_fg;
                Color bright_fg = input_fg;
                if (!has_input_fg && has_input_bg) {
                    bright_fg = dynamic_neutral_gray;
                    bright_has_fg = true;
                }
                bright_pair.has_fg = bright_has_fg;
                bright_pair.fg = bright_fg;
                bright_pair.has_bg = has_input_bg;
                bright_pair.bg = input_bg;
                dark_pair.has_fg = bright_has_fg;
                if (bright_has_fg) {
                    dark_pair.fg = color_adjust_brightness(&bright_fg, 0.2);
                }
                dark_pair.has_bg = has_input_bg;
                if (has_input_bg) {
                    dark_pair.bg = color_adjust_brightness(&input_bg, 0.2);
                }
            } else {
                bright_pair.has_fg = true;
                bright_pair.fg = dynamic_neutral_gray;
                dark_pair.has_fg = true;
                dark_pair.fg = color_adjust_brightness(&dynamic_neutral_gray, 0.2);
            }
        } else {
            const Color *mapped = coordcolormap_get(&mapping, input_coord);
            Color color_bright;
            if (mapped) {
                color_bright = *mapped;
            } else {
                memset(&color_bright, 0, sizeof(color_bright));
            }
            bright_pair.has_fg = true;
            bright_pair.fg = color_bright;
            dark_pair.has_fg = true;
            dark_pair.fg = color_adjust_brightness(&color_bright, 0.2);
        }

        terminal_set_character_visibility(&ctx->terminal, id, true);
        if ((size_t)id < st->map_len) {
            st->color_map[id] = (ColorPair2){.bright = bright_pair, .dark = dark_pair};
            st->color_present[id] = true;
        }
        ch = &ctx->terminal.arena.items[id];
        animation_set_appearance(&ch->animation, uses_pre, input_symbol, &dark_pair);
        free(input_symbol);
    }
    free(characters);

    int64_t smallest_dimension =
        ctx->terminal.canvas.right < ctx->terminal.canvas.top ? ctx->terminal.canvas.right
                                                             : ctx->terminal.canvas.top;
    double range_val = floor((double)smallest_dimension / cfg->beam_width_ratio);
    if (range_val > (double)smallest_dimension) {
        range_val = (double)smallest_dimension;
    }
    st->illuminate_range = (int64_t)range_val;
    if (st->illuminate_range < 1) {
        st->illuminate_range = 1;
    }

    st->search_duration = cfg->search_duration;
    st->searching = true;
    st->expanding = false;
    st->complete = false;

    for (size_t i = 0; i < st->spotlights_len; i++) {
        CharId spotlight = st->spotlights[i];
        engine_activate_path(ctx, self, spotlight, "0");
        ac_insert(&ctx->active_characters, spotlight);
    }

    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    return rc;
}

static const char *spotlights_next_frame(Effect *self, EngineCtx *ctx) {
    Spotlights *st = self->state;
    if (!st->complete) {
        illuminate_chars(st, ctx, st->illuminate_range);
        if (st->searching) {
            st->search_duration -= 1;
            if (st->search_duration == 0) {
                for (size_t i = 0; i < st->spotlights_len; i++) {
                    engine_activate_path(ctx, self, st->spotlights[i], "center");
                }
                st->searching = false;
            }
        }
        bool any_active = false;
        for (size_t i = 0; i < st->spotlights_len; i++) {
            if (ctx->terminal.arena.items[st->spotlights[i]].motion.active_path != NULL) {
                any_active = true;
                break;
            }
        }
        if (!any_active) {
            while (st->spotlights_len > 1) {
                st->spotlights_len--;
            }
            st->expanding = true;
            st->illuminate_range += 1;
            double limit =
                floor((double)(ctx->terminal.canvas.right > ctx->terminal.canvas.top ? ctx->terminal.canvas.right
                                                                                     : ctx->terminal.canvas.top) /
                      1.5);
            if ((double)st->illuminate_range > limit) {
                st->complete = true;
            }
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void spotlights_destroy(Effect *self) {
    Spotlights *st = self->state;
    if (!st) {
        return;
    }
    ac_free(&st->illuminated_chars);
    ac_free(&st->illuminated_scratch);
    free(st->color_map);
    free(st->color_present);
    free(st->spotlights);
    free(st);
    free(self);
}

static const EffectOps SPOTLIGHTS_OPS = {spotlights_build, spotlights_next_frame, spotlights_destroy, NULL};

Effect *spotlights_make(const void *cfg) {
    Spotlights *st = calloc(1, sizeof(Spotlights));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const SpotlightsConfig *)cfg;
    ac_init(&st->illuminated_chars);
    ac_init(&st->illuminated_scratch);
    effect->ops = &SPOTLIGHTS_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec spotlights_specs[] = {
    EF_SPEC("beam-width-ratio", 0, EF_FLOAT_POS, offsetof(SpotlightsConfig, beam_width_ratio)),
    EF_SPEC("beam-falloff", 0, EF_FLOAT_NONNEG, offsetof(SpotlightsConfig, beam_falloff)),
    EF_SPEC("search-duration", 0, EF_POS_INT, offsetof(SpotlightsConfig, search_duration)),
    EF_SPEC("search-speed-range", 0, EF_FLOAT_RANGE, offsetof(SpotlightsConfig, search_speed_range)),
    EF_SPEC("spotlight-count", 0, EF_POS_INT, offsetof(SpotlightsConfig, spotlight_count)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(SpotlightsConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(SpotlightsConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(SpotlightsConfig, final_gradient_direction)),
};

const EffectEntry spotlights_entry = {
    "spotlights",
    spotlights_specs,
    sizeof(spotlights_specs) / sizeof(spotlights_specs[0]),
    sizeof(SpotlightsConfig),
    spotlights_config_defaults,
    spotlights_free_config,
    spotlights_make,
};
