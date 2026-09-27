#include "effects/binarypath.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/render_log.h"
#include "engine/animation.h"
#include "engine/canvas.h"
#include "engine/character.h"
#include "engine/ctx.h"
#include "engine/motion.h"
#include "engine/terminal.h"
#include "utils/easing.h"
#include "utils/geometry.h"
#include "utils/graphics.h"
#include "utils/ordmap.h"

typedef enum {
    BINARY_TRAVEL,
    BINARY_WIPE,
} BinaryPhase;

typedef enum {
    ORIENTATION_COL,
    ORIENTATION_ROW,
} BinaryOrientation;

typedef struct {
    CharId *items;
    size_t len;
    size_t cap;
} BpIdVec;

typedef struct {
    CharId character;
    BpIdVec binary_characters;
    BpIdVec pending_binary_characters;
    Coord input_coord;
    bool is_active;
} BinaryRep;

typedef struct {
    BinaryPathConfig config;
    BinaryRep *pending;
    size_t pending_len;
    size_t pending_cap;
    ColorPair *final_colors;
    bool *final_present;
    size_t map_len;
    bool last_frame_provided;
    BinaryRep *active;
    size_t active_len;
    size_t active_cap;
    bool complete;
    BinaryPhase phase;
    BpIdVec *final_wipe;
    size_t final_wipe_len;
    size_t final_wipe_cap;
    int64_t max_active_binary_groups;
} BinaryPath;

static char *bp_dup(const char *s) {
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

static void bp_idvec_push(BpIdVec *v, CharId id) {
    if (v->len == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 8;
        CharId *grown = realloc(v->items, cap * sizeof(CharId));
        if (!grown) {
            return;
        }
        v->items = grown;
        v->cap = cap;
    }
    v->items[v->len++] = id;
}

static CharId bp_idvec_remove0(BpIdVec *v) {
    CharId value = v->items[0];
    for (size_t i = 1; i < v->len; i++) {
        v->items[i - 1] = v->items[i];
    }
    v->len -= 1;
    return value;
}

static void bp_idvec_free(BpIdVec *v) {
    free(v->items);
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

static void binrep_free(BinaryRep *rep) {
    bp_idvec_free(&rep->binary_characters);
    bp_idvec_free(&rep->pending_binary_characters);
}

static void binrep_push(BinaryRep **arr, size_t *len, size_t *cap, BinaryRep rep) {
    if (*len == *cap) {
        size_t next = *cap ? *cap * 2 : 8;
        BinaryRep *grown = realloc(*arr, next * sizeof(BinaryRep));
        if (!grown) {
            return;
        }
        *arr = grown;
        *cap = next;
    }
    (*arr)[(*len)++] = rep;
}

static BinaryRep binrep_remove(BinaryRep *arr, size_t *len, size_t index) {
    BinaryRep value = arr[index];
    for (size_t i = index + 1; i < *len; i++) {
        arr[i - 1] = arr[i];
    }
    (*len)--;
    memset(&arr[*len], 0, sizeof(BinaryRep));
    return value;
}

// format!(ord(symbol), "08b"): minimum width 8, no upper truncation.
static void codepoint_binary(uint32_t cp, char *out) {
    char tmp[64];
    int len = 0;
    if (cp == 0) {
        tmp[len++] = '0';
    }
    while (cp) {
        tmp[len++] = (char)('0' + (cp & 1u));
        cp >>= 1;
    }
    while (len < 8) {
        tmp[len++] = '0';
    }
    for (int i = 0; i < len; i++) {
        out[i] = tmp[len - 1 - i];
    }
    out[len] = '\0';
}

static uint32_t first_codepoint(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    if (!p || p[0] == 0) {
        return 0;
    }
    if (p[0] < 0x80) {
        return p[0];
    }
    if ((p[0] & 0xE0) == 0xC0) {
        return ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
    }
    if ((p[0] & 0xF0) == 0xE0) {
        return ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) |
               (uint32_t)(p[2] & 0x3F);
    }
    if ((p[0] & 0xF8) == 0xF0) {
        return ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
               ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
    }
    return 0;
}

void binarypath_config_defaults(void *cfg_ptr) {
    BinaryPathConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    push_color_default(&cfg->final_gradient_stops, "00d500");
    push_color_default(&cfg->final_gradient_stops, "007500");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_RADIAL;
    push_color_default(&cfg->binary_colors, "044E29");
    push_color_default(&cfg->binary_colors, "157e38");
    push_color_default(&cfg->binary_colors, "45bf55");
    push_color_default(&cfg->binary_colors, "95ed87");
    cfg->movement_speed = 1.0;
    cfg->active_binary_groups = 0.08;
}

void binarypath_free_config(void *cfg_ptr) {
    BinaryPathConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
    free(cfg->binary_colors.items);
}

static bool binary_rep_travel_complete(const EngineCtx *ctx, const BinaryRep *rep) {
    for (size_t i = 0; i < rep->binary_characters.len; i++) {
        if (!coord_eq(ctx->terminal.arena.items[rep->binary_characters.items[i]].motion.current_coord,
                      rep->input_coord)) {
            return false;
        }
    }
    return true;
}

static BpIdVec bp_group_remove0(BinaryPath *st) {
    BpIdVec value = st->final_wipe[0];
    for (size_t i = 1; i < st->final_wipe_len; i++) {
        st->final_wipe[i - 1] = st->final_wipe[i];
    }
    st->final_wipe_len -= 1;
    return value;
}

static int binarypath_build(Effect *self, EngineCtx *ctx) {
    BinaryPath *st = self->state;
    BinaryPathConfig *cfg = &st->config;
    int rc = 0;

    CharIdGrouping wipe = terminal_get_characters_grouped(&ctx->terminal, character_filter_default(),
                                                          CG_DIAGONAL_TOP_RIGHT_TO_BOTTOM_LEFT);
    st->final_wipe_len = wipe.len;
    st->final_wipe_cap = wipe.len;
    st->final_wipe = calloc(wipe.len ? wipe.len : 1, sizeof(BpIdVec));
    for (size_t i = 0; i < wipe.len; i++) {
        st->final_wipe[i].items = wipe.buckets[i].items;
        st->final_wipe[i].len = wipe.buckets[i].len;
        st->final_wipe[i].cap = wipe.buckets[i].len;
    }
    free(wipe.buckets);

    Gradient final_gradient;
    memset(&final_gradient, 0, sizeof(final_gradient));
    if (gradient_new(cfg->final_gradient_stops.items, cfg->final_gradient_stops.len,
                     cfg->final_gradient_steps.items, cfg->final_gradient_steps.len, false, false,
                     &final_gradient) != 0) {
        return -1;
    }
    CoordColorMap final_gradient_mapping;
    memset(&final_gradient_mapping, 0, sizeof(final_gradient_mapping));
    if (gradient_build_coordinate_color_mapping(&final_gradient, ctx->terminal.canvas.text_bottom,
                                                ctx->terminal.canvas.text_top, ctx->terminal.canvas.text_left,
                                                ctx->terminal.canvas.text_right, cfg->final_gradient_direction,
                                                &final_gradient_mapping) != 0) {
        gradient_free(&final_gradient);
        return -1;
    }

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    CharacterFilter filter = character_filter_default();
    size_t characters_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, filter,
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &characters_len);

    st->map_len = ctx->terminal.arena.len;
    st->final_colors = calloc(st->map_len ? st->map_len : 1, sizeof(ColorPair));
    st->final_present = calloc(st->map_len ? st->map_len : 1, sizeof(bool));

    for (size_t i = 0; i < characters_len; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        ColorPair final_colors;
        memset(&final_colors, 0, sizeof(final_colors));
        if (dynamic) {
            final_colors.has_fg = ch->animation.has_input_fg;
            final_colors.fg = ch->animation.input_fg_color;
            final_colors.has_bg = ch->animation.has_input_bg;
            final_colors.bg = ch->animation.input_bg_color;
        } else {
            const Color *mapped = coordcolormap_get(&final_gradient_mapping, ch->input_coord);
            final_colors.has_fg = true;
            if (mapped) {
                final_colors.fg = *mapped;
            }
        }
        if ((size_t)id < st->map_len) {
            st->final_colors[id] = final_colors;
            st->final_present[id] = true;
        }
    }

    for (size_t i = 0; i < characters_len; i++) {
        CharId id = characters[i];
        const char *symbol = visual_symbol(ctx->terminal.arena.items[id].animation.current_visual);
        Coord input_coord = ctx->terminal.arena.items[id].input_coord;
        uint32_t code_point = first_codepoint(symbol);
        char binary_string[64];
        codepoint_binary(code_point, binary_string);
        BinaryRep rep;
        memset(&rep, 0, sizeof(rep));
        rep.character = id;
        rep.input_coord = input_coord;
        for (size_t k = 0; binary_string[k] != '\0'; k++) {
            char one[2] = {binary_string[k], '\0'};
            CharId added = terminal_add_character(&ctx->terminal, one, coord_new(0, 0));
            bp_idvec_push(&rep.binary_characters, added);
            bp_idvec_push(&rep.pending_binary_characters, added);
        }
        binrep_push(&st->pending, &st->pending_len, &st->pending_cap, rep);
    }

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    for (size_t ri = 0; ri < st->pending_len && rc == 0; ri++) {
        BinaryRep *rep = &st->pending[ri];
        CoordVec path_coords;
        coordvec_init(&path_coords);
        Coord starting_coord = canvas_random_coord(&ctx->terminal.canvas, &ctx->rng, true, false);
        coordvec_push(&path_coords, starting_coord);
        BinaryOrientation last_orientation = (BinaryOrientation)rng_choice_index(&ctx->rng, 2);
        Coord next_coord = starting_coord;
        Coord input_coord = rep->input_coord;
        while (!coord_eq(path_coords.items[path_coords.len - 1], input_coord)) {
            Coord last_coord = path_coords.items[path_coords.len - 1];
            int64_t column_direction = last_coord.column > input_coord.column
                                           ? -1
                                           : (last_coord.column == input_coord.column ? 0 : 1);
            int64_t row_direction = last_coord.row > input_coord.row
                                        ? -1
                                        : (last_coord.row == input_coord.row ? 0 : 1);
            int64_t max_column_distance = llabs(last_coord.column - input_coord.column);
            int64_t max_row_distance = llabs(last_coord.row - input_coord.row);
            if (last_orientation == ORIENTATION_COL && max_row_distance > 0) {
                int64_t fifth = (int64_t)((double)ctx->terminal.canvas.right * 0.2);
                int64_t cap = 10 > fifth ? 10 : fifth;
                int64_t limit = max_row_distance < cap ? max_row_distance : cap;
                int64_t delta = rng_randint(&ctx->rng, 1, limit);
                next_coord = coord_new(last_coord.column, last_coord.row + delta * row_direction);
                last_orientation = ORIENTATION_ROW;
            } else if (last_orientation == ORIENTATION_ROW && max_column_distance > 0) {
                int64_t cap = max_column_distance < 4 ? max_column_distance : 4;
                int64_t delta = rng_randint(&ctx->rng, 1, cap);
                next_coord = coord_new(last_coord.column + delta * column_direction, last_coord.row);
                last_orientation = ORIENTATION_COL;
            } else {
                next_coord = input_coord;
            }
            coordvec_push(&path_coords, next_coord);
        }
        coordvec_push(&path_coords, next_coord);
        coordvec_push(&path_coords, input_coord);

        for (size_t bi = 0; bi < rep->binary_characters.len && rc == 0; bi++) {
            CharId bin_char = rep->binary_characters.items[bi];
            char *path_id = NULL;
            {
                Motion *motion = &ctx->terminal.arena.items[bin_char].motion;
                motion_set_coordinate(motion, path_coords.items[0]);
                if (motion_new_path(motion, cfg->movement_speed, false, no_ease, false, 0, 0, false, "",
                                    &path_id) != 0) {
                    rc = -1;
                    break;
                }
                Path *path = (Path *)om_get(&motion->paths, path_id);
                for (size_t k = 0; k < path_coords.len; k++) {
                    Waypoint wp;
                    memset(&wp, 0, sizeof(wp));
                    if (path_new_waypoint(path, path_coords.items[k], NULL, 0, "", &wp) != 0) {
                        waypoint_free(&wp);
                        rc = -1;
                        break;
                    }
                    waypoint_free(&wp);
                }
                if (rc != 0) {
                    free(path_id);
                    break;
                }
            }
            engine_activate_path(ctx, self, bin_char, path_id);
            ctx->terminal.arena.items[bin_char].layer = 1;
            renderer_layer(bin_char, 1);
            const Color *color =
                &cfg->binary_colors.items[rng_choice_index(&ctx->rng, cfg->binary_colors.len)];
            {
                EffectCharacter *ch = &ctx->terminal.arena.items[bin_char];
                char *symbol = bp_dup(visual_symbol(ch->animation.current_visual));
                bool uses_pre = ch->uses_input_preexisting_colors;
                const char *scene_name = animation_new_scene(
                    &ctx->terminal.arena.items[bin_char].animation, false, false, SYNC_DISTANCE, false,
                    no_ease, "", uses_pre);
                char scene_buf[64];
                scene_buf[0] = '\0';
                if (scene_name) {
                    strncpy(scene_buf, scene_name, sizeof(scene_buf) - 1);
                    scene_buf[sizeof(scene_buf) - 1] = '\0';
                }
                Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[bin_char].animation.scenes,
                                               scene_buf);
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                vp.colors.has_fg = true;
                vp.colors.fg = *color;
                if (scene_add_frame(scene, symbol, 1, &vp) != 0) {
                    rc = -1;
                }
                if (rc == 0) {
                    engine_activate_scene(ctx, self, bin_char, scene_buf);
                }
                free(symbol);
            }
            free(path_id);
        }
        coordvec_free(&path_coords);
    }

    for (size_t i = 0; i < characters_len && rc == 0; i++) {
        CharId id = characters[i];
        char *input_symbol = bp_dup(ctx->terminal.arena.items[id].input_symbol);
        bool uses_pre = ctx->terminal.arena.items[id].uses_input_preexisting_colors;
        ColorPair final_colors;
        memset(&final_colors, 0, sizeof(final_colors));
        if ((size_t)id < st->map_len && st->final_present[id]) {
            final_colors = st->final_colors[id];
        }
        bool has_final_fg = final_colors.has_fg;
        bool has_final_bg = final_colors.has_bg;
        Color final_fg = final_colors.fg;
        Color final_bg = final_colors.bg;
        bool has_dim_fg = has_final_fg;
        bool has_dim_bg = has_final_bg;
        Color dim_fg;
        Color dim_bg;
        memset(&dim_fg, 0, sizeof(dim_fg));
        memset(&dim_bg, 0, sizeof(dim_bg));
        if (has_dim_fg) {
            dim_fg = color_adjust_brightness(&final_fg, 0.5);
        }
        if (has_dim_bg) {
            dim_bg = color_adjust_brightness(&final_bg, 0.5);
        }
        Color white;
        color_from_hex("ffffff", &white);

        Gradient collapse_fg;
        Gradient collapse_bg;
        bool has_collapse_fg = false;
        bool has_collapse_bg = false;
        memset(&collapse_fg, 0, sizeof(collapse_fg));
        memset(&collapse_bg, 0, sizeof(collapse_bg));
        if (has_dim_fg) {
            Color stops[2] = {white, dim_fg};
            if (gradient_with_steps(stops, 2, 7, false, &collapse_fg) == 0) {
                has_collapse_fg = true;
            } else {
                rc = -1;
            }
        }
        if (rc == 0 && has_dim_bg) {
            Color stops[2] = {white, dim_bg};
            if (gradient_with_steps(stops, 2, 7, false, &collapse_bg) == 0) {
                has_collapse_bg = true;
            } else {
                rc = -1;
            }
        }

        if (rc == 0) {
            const char *collapse_name = animation_new_scene(
                &ctx->terminal.arena.items[id].animation, false, false, SYNC_DISTANCE, true,
                easing_named(EASE_IN_QUAD), "collapse_scn", uses_pre);
            (void)collapse_name;
            Scene *scene =
                (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "collapse_scn");
            if (has_collapse_fg || has_collapse_bg) {
                const char *syms[1] = {input_symbol};
                if (scene_apply_gradient_to_symbols(scene, syms, 1, 3,
                                                    has_collapse_fg ? &collapse_fg : NULL,
                                                    has_collapse_bg ? &collapse_bg : NULL) != 0) {
                    rc = -1;
                }
            } else {
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                if (scene_add_frame(scene, input_symbol, 3, &vp) != 0) {
                    rc = -1;
                }
            }
        }

        bool has_brighten_fg = has_dim_fg && has_final_fg;
        bool has_brighten_bg = has_dim_bg && has_final_bg;
        Gradient brighten_fg;
        Gradient brighten_bg;
        bool has_brighten_fg_gradient = false;
        bool has_brighten_bg_gradient = false;
        memset(&brighten_fg, 0, sizeof(brighten_fg));
        memset(&brighten_bg, 0, sizeof(brighten_bg));
        if (rc == 0 && has_brighten_fg) {
            Color stops[2] = {dim_fg, final_fg};
            if (gradient_with_steps(stops, 2, 10, false, &brighten_fg) == 0) {
                has_brighten_fg_gradient = true;
            } else {
                rc = -1;
            }
        }
        if (rc == 0 && has_brighten_bg) {
            Color stops[2] = {dim_bg, final_bg};
            if (gradient_with_steps(stops, 2, 10, false, &brighten_bg) == 0) {
                has_brighten_bg_gradient = true;
            } else {
                rc = -1;
            }
        }

        if (rc == 0) {
            const char *brighten_name = animation_new_scene(
                &ctx->terminal.arena.items[id].animation, false, false, SYNC_DISTANCE, false, no_ease,
                "brighten_scn", uses_pre);
            (void)brighten_name;
            Scene *scene =
                (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "brighten_scn");
            if (has_brighten_fg_gradient || has_brighten_bg_gradient) {
                const char *syms[1] = {input_symbol};
                if (scene_apply_gradient_to_symbols(scene, syms, 1, 2,
                                                    has_brighten_fg_gradient ? &brighten_fg : NULL,
                                                    has_brighten_bg_gradient ? &brighten_bg : NULL) != 0) {
                    rc = -1;
                }
            } else {
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                if (scene_add_frame(scene, input_symbol, 2, &vp) != 0) {
                    rc = -1;
                }
            }
        }

        if (has_collapse_fg) {
            gradient_free(&collapse_fg);
        }
        if (has_collapse_bg) {
            gradient_free(&collapse_bg);
        }
        if (has_brighten_fg_gradient) {
            gradient_free(&brighten_fg);
        }
        if (has_brighten_bg_gradient) {
            gradient_free(&brighten_bg);
        }
        free(input_symbol);
    }

    st->max_active_binary_groups =
        (int64_t)(cfg->active_binary_groups * (double)st->pending_len);
    if (st->max_active_binary_groups < 1) {
        st->max_active_binary_groups = 1;
    }

    free(characters);
    coordcolormap_free(&final_gradient_mapping);
    gradient_free(&final_gradient);
    st->phase = BINARY_TRAVEL;
    return rc;
}

static const char *binarypath_next_frame(Effect *self, EngineCtx *ctx) {
    BinaryPath *st = self->state;
    if (!st->complete || !ac_is_empty(&ctx->active_characters)) {
        if (st->phase == BINARY_TRAVEL) {
            while ((int64_t)st->active_len < st->max_active_binary_groups && st->pending_len > 0) {
                int64_t index = rng_randrange(&ctx->rng, 0, (int64_t)st->pending_len);
                BinaryRep next_rep = binrep_remove(st->pending, &st->pending_len, (size_t)index);
                next_rep.is_active = true;
                binrep_push(&st->active, &st->active_len, &st->active_cap, next_rep);
            }

            if (st->active_len > 0) {
                BinaryRep *active = st->active;
                size_t active_len = st->active_len;
                st->active = NULL;
                st->active_len = 0;
                st->active_cap = 0;
                for (size_t i = 0; i < active_len; i++) {
                    BinaryRep *rep = &active[i];
                    if (rep->pending_binary_characters.len > 0) {
                        CharId next_char = bp_idvec_remove0(&rep->pending_binary_characters);
                        ac_insert(&ctx->active_characters, next_char);
                        terminal_set_character_visibility(&ctx->terminal, next_char, true);
                    } else if (binary_rep_travel_complete(ctx, rep)) {
                        for (size_t k = 0; k < rep->binary_characters.len; k++) {
                            terminal_set_character_visibility(
                                &ctx->terminal, rep->binary_characters.items[k], false);
                        }
                        rep->is_active = false;
                        terminal_set_character_visibility(&ctx->terminal, rep->character, true);
                        engine_activate_scene(ctx, self, rep->character, "collapse_scn");
                        ac_insert(&ctx->active_characters, rep->character);
                    }
                }
                size_t keep = 0;
                for (size_t i = 0; i < active_len; i++) {
                    if (active[i].is_active) {
                        if (keep != i) {
                            active[keep] = active[i];
                        }
                        keep++;
                    } else {
                        binrep_free(&active[i]);
                    }
                }
                st->active = active;
                st->active_len = keep;
                st->active_cap = active_len;
            }

            if (ac_is_empty(&ctx->active_characters)) {
                st->phase = BINARY_WIPE;
            }
        }

        if (st->phase == BINARY_WIPE) {
            for (int k = 0; k < 2; k++) {
                if (st->final_wipe_len > 0) {
                    BpIdVec next_group = bp_group_remove0(st);
                    for (size_t j = 0; j < next_group.len; j++) {
                        CharId character = next_group.items[j];
                        engine_activate_scene(ctx, self, character, "brighten_scn");
                        terminal_set_character_visibility(&ctx->terminal, character, true);
                        ac_insert(&ctx->active_characters, character);
                    }
                    free(next_group.items);
                } else {
                    st->complete = true;
                }
            }
        }

        engine_update(ctx, self);
        return engine_frame(ctx);
    }

    if (!st->last_frame_provided) {
        st->last_frame_provided = true;
        return engine_frame(ctx);
    }

    return NULL;
}

static void binarypath_destroy(Effect *self) {
    BinaryPath *st = self->state;
    if (!st) {
        return;
    }
    for (size_t i = 0; i < st->pending_len; i++) {
        binrep_free(&st->pending[i]);
    }
    free(st->pending);
    for (size_t i = 0; i < st->active_len; i++) {
        binrep_free(&st->active[i]);
    }
    free(st->active);
    for (size_t i = 0; i < st->final_wipe_len; i++) {
        free(st->final_wipe[i].items);
    }
    free(st->final_wipe);
    free(st->final_colors);
    free(st->final_present);
    free(st);
    free(self);
}

static const EffectOps BINARYPATH_OPS = {binarypath_build, binarypath_next_frame, binarypath_destroy,
                                         NULL};

Effect *binarypath_make(const void *cfg) {
    BinaryPath *st = calloc(1, sizeof(BinaryPath));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const BinaryPathConfig *)cfg;
    st->phase = BINARY_TRAVEL;
    effect->ops = &BINARYPATH_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec binarypath_specs[] = {
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(BinaryPathConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(BinaryPathConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(BinaryPathConfig, final_gradient_direction)),
    EF_SPEC("binary-colors", 0, EF_COLOR_LIST, offsetof(BinaryPathConfig, binary_colors)),
    EF_SPEC("movement-speed", 0, EF_FLOAT_POS, offsetof(BinaryPathConfig, movement_speed)),
    EF_SPEC("active-binary-groups", 0, EF_RATIO_NONNEG, offsetof(BinaryPathConfig, active_binary_groups)),
};

const EffectEntry binarypath_entry = {
    "binarypath",
    binarypath_specs,
    sizeof(binarypath_specs) / sizeof(binarypath_specs[0]),
    sizeof(BinaryPathConfig),
    binarypath_config_defaults,
    binarypath_free_config,
    binarypath_make,
};
