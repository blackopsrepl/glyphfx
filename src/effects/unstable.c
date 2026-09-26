#include "effects/unstable.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/canvas.h"
#include "engine/character.h"
#include "engine/ctx.h"
#include "engine/motion.h"
#include "engine/terminal.h"
#include "utils/geometry.h"
#include "utils/graphics.h"

typedef enum {
    UNSTABLE_RUMBLE,
    UNSTABLE_EXPLOSION,
    UNSTABLE_REASSEMBLY,
} UnstablePhase;

typedef struct {
    UnstableConfig config;
    // HashMap<CharId, Coord>.
    Coord *jumbled_coords;
    bool *jumbled_present;
    size_t jumbled_len;
    // HashMap<CharId, ColorPair>.
    ColorPair *character_final_color_map;
    bool *final_present;
    ColorPair *character_start_color_map;
    bool *start_present;
    size_t colors_len;
    int64_t explosion_hold_time;
    UnstablePhase phase;
    int64_t max_rumble_steps;
    int64_t current_rumble_steps;
    int64_t rumble_mod_delay;
} Unstable;

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

static Color neutral_gray(void) {
    Color c;
    color_from_hex("808080", &c);
    return c;
}

void unstable_config_defaults(void *cfg_ptr) {
    UnstableConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    color_from_hex("ff9200", &cfg->unstable_color);
    easing_parse("out_expo", &cfg->explosion_ease);
    cfg->explosion_speed = 1.0;
    easing_parse("out_expo", &cfg->reassembly_ease);
    cfg->reassembly_speed = 1.0;
    push_color_default(&cfg->final_gradient_stops, "8A008A");
    push_color_default(&cfg->final_gradient_stops, "00D1FF");
    push_color_default(&cfg->final_gradient_stops, "FFFFFF");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void unstable_free_config(void *cfg_ptr) {
    UnstableConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

// Vec::remove.
static Coord coord_vec_remove(Coord *arr, size_t *len, size_t index) {
    Coord value = arr[index];
    for (size_t i = index; i + 1 < *len; i++) {
        arr[i] = arr[i + 1];
    }
    *len -= 1;
    return value;
}

static void mask_pair(ColorPair *out, bool has_fg, Color fg, bool has_bg, Color bg) {
    memset(out, 0, sizeof(*out));
    out->has_fg = has_fg;
    out->fg = fg;
    out->has_bg = has_bg;
    out->bg = bg;
}

static int unstable_build(Effect *self, EngineCtx *ctx) {
    Unstable *st = self->state;
    UnstableConfig *cfg = &st->config;

    Gradient final_gradient;
    if (gradient_new(cfg->final_gradient_stops.items, cfg->final_gradient_stops.len,
                     cfg->final_gradient_steps.items, cfg->final_gradient_steps.len, false, false,
                     &final_gradient) != 0) {
        return -1;
    }
    CoordColorMap final_gradient_mapping;
    if (gradient_build_coordinate_color_mapping(&final_gradient, ctx->terminal.canvas.text_bottom,
                                                ctx->terminal.canvas.text_top, ctx->terminal.canvas.text_left,
                                                ctx->terminal.canvas.text_right,
                                                cfg->final_gradient_direction,
                                                &final_gradient_mapping) != 0) {
        gradient_free(&final_gradient);
        return -1;
    }

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    size_t characters_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &characters_len);

    size_t arena_len = ctx->terminal.arena.len;
    st->colors_len = arena_len;
    st->jumbled_len = arena_len;
    st->jumbled_coords = calloc(arena_len ? arena_len : 1, sizeof(Coord));
    st->jumbled_present = calloc(arena_len ? arena_len : 1, sizeof(bool));
    st->character_final_color_map = calloc(arena_len ? arena_len : 1, sizeof(ColorPair));
    st->final_present = calloc(arena_len ? arena_len : 1, sizeof(bool));
    st->character_start_color_map = calloc(arena_len ? arena_len : 1, sizeof(ColorPair));
    st->start_present = calloc(arena_len ? arena_len : 1, sizeof(bool));

    Color ngray = neutral_gray();

    for (size_t i = 0; i < characters_len; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        ColorPair start_colors;
        ColorPair final_colors;
        if (dynamic) {
            Color start_fg = ch->animation.has_input_fg ? ch->animation.input_fg_color : ngray;
            mask_pair(&start_colors, true, start_fg, ch->animation.has_input_bg, ch->animation.input_bg_color);
            mask_pair(&final_colors, ch->animation.has_input_fg, ch->animation.input_fg_color,
                      ch->animation.has_input_bg, ch->animation.input_bg_color);
        } else {
            const Color *mapped = coordcolormap_get(&final_gradient_mapping, ch->input_coord);
            Color fc = mapped ? *mapped : ngray;
            mask_pair(&start_colors, true, fc, false, ngray);
            final_colors = start_colors;
        }
        if ((size_t)id < st->colors_len) {
            st->character_start_color_map[id] = start_colors;
            st->start_present[id] = true;
            st->character_final_color_map[id] = final_colors;
            st->final_present[id] = true;
        }
    }

    Coord *character_coords = malloc((characters_len ? characters_len : 1) * sizeof(Coord));
    size_t coords_len = characters_len;
    for (size_t i = 0; i < characters_len; i++) {
        character_coords[i] = ctx->terminal.arena.items[characters[i]].input_coord;
    }

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    int rc = 0;
    for (size_t i = 0; i < characters_len && rc == 0; i++) {
        CharId id = characters[i];
        int64_t pos = rng_randint(&ctx->rng, 0, 3);
        int64_t col;
        int64_t row;
        if (pos == 0) {
            col = ctx->terminal.canvas.left;
            row = canvas_random_row(&ctx->terminal.canvas, &ctx->rng, false);
        } else if (pos == 1) {
            col = ctx->terminal.canvas.right;
            row = canvas_random_row(&ctx->terminal.canvas, &ctx->rng, false);
        } else if (pos == 2) {
            col = canvas_random_column(&ctx->terminal.canvas, &ctx->rng, false);
            row = ctx->terminal.canvas.bottom;
        } else {
            col = canvas_random_column(&ctx->terminal.canvas, &ctx->rng, false);
            row = ctx->terminal.canvas.top;
        }
        int64_t remove_index = rng_randint(&ctx->rng, 0, (int64_t)coords_len - 1);
        Coord jumbled_coord = coord_vec_remove(character_coords, &coords_len, (size_t)remove_index);
        if ((size_t)id < st->jumbled_len) {
            st->jumbled_coords[id] = jumbled_coord;
            st->jumbled_present[id] = true;
        }

        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        bool uses_pre = ch->uses_input_preexisting_colors;
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);
        Coord input_coord = ch->input_coord;
        motion_set_coordinate(&ch->motion, jumbled_coord);

        char *explosion_path = NULL;
        if (motion_new_path(&ch->motion, cfg->explosion_speed, true, cfg->explosion_ease, false, 0, 0, false,
                            "explosion", &explosion_path) != 0) {
            free(input_symbol);
            rc = -1;
            break;
        }
        Path *ep = (Path *)om_get(&ch->motion.paths, explosion_path);
        Waypoint wp;
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(ep, coord_new(col, row), NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(explosion_path);
            free(input_symbol);
            rc = -1;
            break;
        }
        waypoint_free(&wp);

        char *reassembly_path = NULL;
        if (motion_new_path(&ch->motion, cfg->reassembly_speed, true, cfg->reassembly_ease, false, 0, 0, false,
                            "reassembly", &reassembly_path) != 0) {
            free(explosion_path);
            free(input_symbol);
            rc = -1;
            break;
        }
        Path *rp = (Path *)om_get(&ch->motion.paths, reassembly_path);
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(rp, input_coord, NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(explosion_path);
            free(reassembly_path);
            free(input_symbol);
            rc = -1;
            break;
        }
        waypoint_free(&wp);
        free(explosion_path);
        free(reassembly_path);

        animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "rumble", uses_pre);

        if (dynamic) {
            ColorPair start_pair = st->character_start_color_map[id];
            Color start_fg = start_pair.has_fg ? start_pair.fg : ngray;
            Color stops[2] = {start_fg, cfg->unstable_color};
            Gradient fg_gradient;
            Gradient bg_gradient;
            bool has_bg_gradient = false;
            if (gradient_with_steps(stops, 2, 12, false, &fg_gradient) != 0) {
                free(input_symbol);
                rc = -1;
                break;
            }
            if (start_pair.has_bg) {
                Color bg_stops[2] = {start_pair.bg, cfg->unstable_color};
                if (gradient_with_steps(bg_stops, 2, 12, false, &bg_gradient) != 0) {
                    gradient_free(&fg_gradient);
                    free(input_symbol);
                    rc = -1;
                    break;
                }
                has_bg_gradient = true;
            }
            Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "rumble");
            const char *syms[1] = {input_symbol};
            if (scene_apply_gradient_to_symbols(scene, syms, 1, 10, &fg_gradient,
                                                has_bg_gradient ? &bg_gradient : NULL) != 0) {
                rc = -1;
            }
            gradient_free(&fg_gradient);
            if (has_bg_gradient) {
                gradient_free(&bg_gradient);
            }
        } else {
            ColorPair final_pair = st->character_final_color_map[id];
            Color final_fg = final_pair.has_fg ? final_pair.fg : ngray;
            Color stops[2] = {final_fg, cfg->unstable_color};
            Gradient unstable_gradient;
            if (gradient_with_steps(stops, 2, 12, false, &unstable_gradient) != 0) {
                free(input_symbol);
                rc = -1;
                break;
            }
            Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "rumble");
            const char *syms[1] = {input_symbol};
            if (scene_apply_gradient_to_symbols(scene, syms, 1, 10, &unstable_gradient, NULL) != 0) {
                rc = -1;
            }
            gradient_free(&unstable_gradient);
        }
        if (rc != 0) {
            free(input_symbol);
            break;
        }

        {
            EffectCharacter *c2 = &ctx->terminal.arena.items[id];
            animation_new_scene(&c2->animation, false, false, SYNC_DISTANCE, false, no_ease, "final", uses_pre);
        }

        if (dynamic) {
            ColorPair final_pair = st->character_final_color_map[id];
            bool has_ffg = final_pair.has_fg;
            Color final_fg = final_pair.fg;
            bool has_fbg = final_pair.has_bg;
            Color final_bg = final_pair.bg;
            if (!has_ffg && !has_fbg) {
                Color stops[2] = {cfg->unstable_color, ngray};
                Gradient fg_gradient;
                if (gradient_with_steps(stops, 2, 12, false, &fg_gradient) != 0) {
                    free(input_symbol);
                    rc = -1;
                    break;
                }
                Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "final");
                const char *syms[1] = {input_symbol};
                if (scene_apply_gradient_to_symbols(scene, syms, 1, 3, &fg_gradient, NULL) != 0) {
                    rc = -1;
                }
                gradient_free(&fg_gradient);
                VisualParams vp;
                memset(&vp, 0, sizeof(vp));
                vp.has_colors = true;
                if (scene_add_frame(scene, input_symbol, 3, &vp) != 0) {
                    rc = -1;
                }
            } else {
                Gradient fg_gradient;
                Gradient bg_gradient;
                bool has_fg_gradient = false;
                bool has_bg_gradient = false;
                if (has_ffg) {
                    Color stops[2] = {cfg->unstable_color, final_fg};
                    if (gradient_with_steps(stops, 2, 12, false, &fg_gradient) != 0) {
                        free(input_symbol);
                        rc = -1;
                        break;
                    }
                    has_fg_gradient = true;
                }
                if (has_fbg) {
                    Color stops[2] = {cfg->unstable_color, final_bg};
                    if (gradient_with_steps(stops, 2, 12, false, &bg_gradient) != 0) {
                        if (has_fg_gradient) {
                            gradient_free(&fg_gradient);
                        }
                        free(input_symbol);
                        rc = -1;
                        break;
                    }
                    has_bg_gradient = true;
                }
                Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "final");
                const char *syms[1] = {input_symbol};
                if (scene_apply_gradient_to_symbols(scene, syms, 1, 3,
                                                    has_fg_gradient ? &fg_gradient : NULL,
                                                    has_bg_gradient ? &bg_gradient : NULL) != 0) {
                    rc = -1;
                }
                if (rc == 0 && !has_ffg) {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    vp.colors.has_bg = true;
                    vp.colors.bg = final_bg;
                    if (scene_add_frame(scene, input_symbol, 3, &vp) != 0) {
                        rc = -1;
                    }
                }
                if (has_fg_gradient) {
                    gradient_free(&fg_gradient);
                }
                if (has_bg_gradient) {
                    gradient_free(&bg_gradient);
                }
            }
        } else {
            ColorPair final_pair = st->character_final_color_map[id];
            Color final_fg = final_pair.has_fg ? final_pair.fg : ngray;
            Color stops[2] = {cfg->unstable_color, final_fg};
            Gradient final_color;
            if (gradient_with_steps(stops, 2, 12, false, &final_color) != 0) {
                free(input_symbol);
                rc = -1;
                break;
            }
            Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, "final");
            const char *syms[1] = {input_symbol};
            if (scene_apply_gradient_to_symbols(scene, syms, 1, 3, &final_color, NULL) != 0) {
                rc = -1;
            }
            gradient_free(&final_color);
        }
        if (rc != 0) {
            free(input_symbol);
            break;
        }

        engine_activate_scene(ctx, self, id, "rumble");
        if (dynamic) {
            EffectCharacter *c2 = &ctx->terminal.arena.items[id];
            ColorPair start_pair = st->character_start_color_map[id];
            animation_set_appearance(&c2->animation, c2->uses_input_preexisting_colors, input_symbol,
                                     &start_pair);
        }
        terminal_set_character_visibility(&ctx->terminal, id, true);
        free(input_symbol);
    }

    free(characters);
    free(character_coords);
    coordcolormap_free(&final_gradient_mapping);
    gradient_free(&final_gradient);

    if (rc == 0) {
        st->explosion_hold_time = 30;
        st->phase = UNSTABLE_RUMBLE;
        st->max_rumble_steps = 150;
        st->current_rumble_steps = 0;
        st->rumble_mod_delay = 18;
    }
    return rc;
}

static void unstable_retain_by_predicate(EngineCtx *ctx, bool reassembly) {
    CharId *snapshot = NULL;
    size_t snapshot_len = 0;
    ac_snapshot(&ctx->active_characters, &snapshot, &snapshot_len);
    CharId *retained = malloc((snapshot_len ? snapshot_len : 1) * sizeof(CharId));
    size_t retained_len = 0;
    for (size_t i = 0; i < snapshot_len; i++) {
        CharId id = snapshot[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        const char *path_id = reassembly ? "reassembly" : "explosion";
        Path *p = (Path *)om_get(&ch->motion.paths, path_id);
        if (!p || p->waypoints_len == 0) {
            continue;
        }
        Coord target = p->waypoints[0].coord;
        bool keep;
        if (reassembly) {
            keep = !coord_eq(ch->motion.current_coord, target) ||
                   !animation_active_scene_is_complete(&ch->animation);
        } else {
            keep = !coord_eq(ch->motion.current_coord, target);
        }
        if (keep) {
            retained[retained_len++] = id;
        }
    }
    free(snapshot);
    ac_clear(&ctx->active_characters);
    ac_extend(&ctx->active_characters, retained, retained_len);
    free(retained);
}

static const char *unstable_next_frame(Effect *self, EngineCtx *ctx) {
    Unstable *st = self->state;
    const char *next_frame = NULL;

    if (st->phase == UNSTABLE_RUMBLE) {
        if (st->current_rumble_steps < st->max_rumble_steps) {
            if (st->current_rumble_steps > 30 && st->current_rumble_steps % st->rumble_mod_delay == 0) {
                static const int64_t offsets[3] = {-1, 0, 1};
                int64_t row_offset = offsets[rng_choice_index(&ctx->rng, 3)];
                int64_t column_offset = offsets[rng_choice_index(&ctx->rng, 3)];
                size_t characters_len = 0;
                CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng,
                                                             character_filter_default(),
                                                             CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &characters_len);
                for (size_t i = 0; i < characters_len; i++) {
                    CharId id = characters[i];
                    Motion *motion = &ctx->terminal.arena.items[id].motion;
                    Coord current = motion->current_coord;
                    motion_set_coordinate(motion, coord_new(current.column + column_offset,
                                                            current.row + row_offset));
                    engine_step_animation(ctx, self, id);
                }
                next_frame = engine_frame(ctx);
                for (size_t i = 0; i < characters_len; i++) {
                    CharId id = characters[i];
                    Coord jumbled = st->jumbled_coords[id];
                    motion_set_coordinate(&ctx->terminal.arena.items[id].motion, jumbled);
                }
                st->rumble_mod_delay -= 1;
                if (st->rumble_mod_delay < 1) {
                    st->rumble_mod_delay = 1;
                }
                free(characters);
            } else {
                size_t characters_len = 0;
                CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng,
                                                             character_filter_default(),
                                                             CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &characters_len);
                for (size_t i = 0; i < characters_len; i++) {
                    engine_step_animation(ctx, self, characters[i]);
                }
                next_frame = engine_frame(ctx);
                free(characters);
            }
            st->current_rumble_steps += 1;
        } else {
            st->phase = UNSTABLE_EXPLOSION;
            size_t characters_len = 0;
            CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng,
                                                         character_filter_default(),
                                                         CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &characters_len);
            for (size_t i = 0; i < characters_len; i++) {
                engine_activate_path(ctx, self, characters[i], "explosion");
            }
            ac_clear(&ctx->active_characters);
            ac_extend(&ctx->active_characters, characters, characters_len);
            free(characters);
        }
    }

    if (st->phase == UNSTABLE_EXPLOSION) {
        if (!ac_is_empty(&ctx->active_characters)) {
            CharId *snapshot = NULL;
            size_t snapshot_len = 0;
            ac_snapshot(&ctx->active_characters, &snapshot, &snapshot_len);
            for (size_t i = 0; i < snapshot_len; i++) {
                engine_tick(ctx, self, snapshot[i]);
            }
            free(snapshot);
            unstable_retain_by_predicate(ctx, false);
            next_frame = engine_frame(ctx);
        } else if (st->explosion_hold_time != 0) {
            st->explosion_hold_time -= 1;
            next_frame = engine_frame(ctx);
        } else {
            st->phase = UNSTABLE_REASSEMBLY;
            size_t characters_len = 0;
            CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng,
                                                         character_filter_default(),
                                                         CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &characters_len);
            for (size_t i = 0; i < characters_len; i++) {
                CharId id = characters[i];
                engine_activate_scene(ctx, self, id, "final");
                ac_insert(&ctx->active_characters, id);
                engine_activate_path(ctx, self, id, "reassembly");
            }
            free(characters);
        }
    }

    if (st->phase == UNSTABLE_REASSEMBLY && !ac_is_empty(&ctx->active_characters)) {
        CharId *snapshot = NULL;
        size_t snapshot_len = 0;
        ac_snapshot(&ctx->active_characters, &snapshot, &snapshot_len);
        for (size_t i = 0; i < snapshot_len; i++) {
            engine_tick(ctx, self, snapshot[i]);
        }
        free(snapshot);
        unstable_retain_by_predicate(ctx, true);
        next_frame = engine_frame(ctx);
    }

    return next_frame;
}

static void unstable_destroy(Effect *self) {
    Unstable *st = self->state;
    if (!st) {
        return;
    }
    free(st->jumbled_coords);
    free(st->jumbled_present);
    free(st->character_final_color_map);
    free(st->final_present);
    free(st->character_start_color_map);
    free(st->start_present);
    free(st);
    free(self);
}

static const EffectOps UNSTABLE_OPS = {unstable_build, unstable_next_frame, unstable_destroy, NULL};

Effect *unstable_make(const void *cfg) {
    Unstable *st = calloc(1, sizeof(Unstable));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const UnstableConfig *)cfg;
    effect->ops = &UNSTABLE_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec unstable_specs[] = {
    EF_SPEC("unstable-color", 0, EF_COLOR, offsetof(UnstableConfig, unstable_color)),
    EF_SPEC("explosion-ease", 0, EF_EASING, offsetof(UnstableConfig, explosion_ease)),
    EF_SPEC("explosion-speed", 0, EF_FLOAT_POS, offsetof(UnstableConfig, explosion_speed)),
    EF_SPEC("reassembly-ease", 0, EF_EASING, offsetof(UnstableConfig, reassembly_ease)),
    EF_SPEC("reassembly-speed", 0, EF_FLOAT_POS, offsetof(UnstableConfig, reassembly_speed)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(UnstableConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(UnstableConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(UnstableConfig, final_gradient_direction)),
};

const EffectEntry unstable_entry = {
    "unstable",
    unstable_specs,
    sizeof(unstable_specs) / sizeof(unstable_specs[0]),
    sizeof(UnstableConfig),
    unstable_config_defaults,
    unstable_free_config,
    unstable_make,
};
