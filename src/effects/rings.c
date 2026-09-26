#include "effects/rings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/canvas.h"
#include "engine/ctx.h"
#include "engine/events.h"
#include "engine/motion.h"
#include "engine/terminal.h"
#include "utils/easing.h"
#include "utils/geometry.h"
#include "utils/graphics.h"
#include "utils/ordmap.h"
#include "utils/pycompat.h"

#define CB_SET_INVISIBLE 0u

typedef enum {
    RINGS_START,
    RINGS_DISPERSE,
    RINGS_SPIN,
    RINGS_FINAL,
    RINGS_COMPLETE,
} RingsPhase;

typedef struct {
    CharId *items;
    size_t len;
    size_t cap;
} RingIdVec;

typedef struct {
    CharId *keys;
    char **vals;
    size_t len;
    size_t cap;
} RingPathMap;

typedef struct {
    int64_t radius;
    Coord origin;
    Coord *counter_clockwise;
    size_t cc_len;
    Coord *clockwise;
    size_t cw_len;
    int64_t ring_gap;
    Color ring_color;
    RingIdVec characters;
    RingPathMap last_path;
    double rotation_speed;
} Ring;

typedef struct {
    RingsConfig config;
    Ring *rings;
    size_t rings_len;
    size_t rings_cap;
    RingIdVec non_ring_chars;
    ColorPair *final_colors;
    bool *final_present;
    size_t map_len;
    RingsPhase phase;
    bool initial_disperse_complete;
    int64_t spin_time_remaining;
    int64_t disperse_time_remaining;
    int64_t cycles_remaining;
    int64_t initial_phase_time_remaining;
} Rings;

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

static void idvec_push(RingIdVec *v, CharId id) {
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

static void idvec_free(RingIdVec *v) {
    free(v->items);
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

static void idvec_clone(RingIdVec *dst, const RingIdVec *src) {
    dst->items = NULL;
    dst->len = 0;
    dst->cap = 0;
    for (size_t i = 0; i < src->len; i++) {
        idvec_push(dst, src->items[i]);
    }
}

static void ring_map_free(RingPathMap *m) {
    for (size_t i = 0; i < m->len; i++) {
        free(m->vals[i]);
    }
    free(m->keys);
    free(m->vals);
    m->keys = NULL;
    m->vals = NULL;
    m->len = 0;
    m->cap = 0;
}

static void ring_map_set(RingPathMap *m, CharId id, const char *value) {
    for (size_t i = 0; i < m->len; i++) {
        if (m->keys[i] == id) {
            free(m->vals[i]);
            m->vals[i] = malloc(strlen(value) + 1);
            strcpy(m->vals[i], value);
            return;
        }
    }
    if (m->len == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 8;
        m->keys = realloc(m->keys, cap * sizeof(CharId));
        m->vals = realloc(m->vals, cap * sizeof(char *));
        m->cap = cap;
    }
    m->keys[m->len] = id;
    m->vals[m->len] = malloc(strlen(value) + 1);
    strcpy(m->vals[m->len], value);
    m->len++;
}

static const char *ring_map_get(const RingPathMap *m, CharId id) {
    for (size_t i = 0; i < m->len; i++) {
        if (m->keys[i] == id) {
            return m->vals[i];
        }
    }
    return NULL;
}

static int gradient_pair(Color a, Color b, int64_t steps, Gradient *out) {
    Color stops[2] = {a, b};
    return gradient_with_steps(stops, 2, steps, false, out);
}

void rings_config_defaults(void *cfg_ptr) {
    RingsConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    push_color_default(&cfg->ring_colors, "ab48ff");
    push_color_default(&cfg->ring_colors, "e7b2b2");
    push_color_default(&cfg->ring_colors, "fffebd");
    cfg->ring_gap = 0.1;
    cfg->spin_duration = 200;
    cfg->spin_speed.start = 0.25;
    cfg->spin_speed.end = 1.0;
    cfg->disperse_duration = 200;
    cfg->spin_disperse_cycles = 3;
    push_color_default(&cfg->final_gradient_stops, "ab48ff");
    push_color_default(&cfg->final_gradient_stops, "e7b2b2");
    push_color_default(&cfg->final_gradient_stops, "fffebd");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void rings_free_config(void *cfg_ptr) {
    RingsConfig *cfg = cfg_ptr;
    free(cfg->ring_colors.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static Ring ring_new(EngineCtx *ctx, const RingsConfig *cfg, int64_t radius, Coord origin, CoordVec ring_coords,
                     int64_t ring_gap, Color ring_color) {
    Ring ring;
    memset(&ring, 0, sizeof(ring));
    ring.radius = radius;
    ring.origin = origin;
    ring.counter_clockwise = ring_coords.items;
    ring.cc_len = ring_coords.len;
    ring.clockwise = malloc((ring_coords.len ? ring_coords.len : 1) * sizeof(Coord));
    for (size_t i = 0; i < ring_coords.len; i++) {
        ring.clockwise[i] = ring_coords.items[ring_coords.len - 1 - i];
    }
    ring.cw_len = ring_coords.len;
    ring.ring_gap = ring_gap;
    ring.ring_color = ring_color;
    ring.rotation_speed = rng_uniform(&ctx->rng, cfg->spin_speed.start, cfg->spin_speed.end);
    return ring;
}

static void ring_free(Ring *ring) {
    free(ring->counter_clockwise);
    free(ring->clockwise);
    idvec_free(&ring->characters);
    ring_map_free(&ring->last_path);
    ring->counter_clockwise = NULL;
    ring->clockwise = NULL;
}

static const char *ring_make_disperse_waypoints(EngineCtx *ctx, Ring *ring, CharId id, Coord origin) {
    CoordVec disperse_coords = find_coords_in_rect(origin, ring->ring_gap);
    Coord waypoint_coords[5];
    for (int i = 0; i < 5; i++) {
        int64_t index = rng_randrange(&ctx->rng, 0, (int64_t)disperse_coords.len);
        waypoint_coords[i] = disperse_coords.items[(size_t)index];
    }
    coordvec_free(&disperse_coords);

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));
    Motion *motion = &ctx->terminal.arena.items[id].motion;
    Path *existing = (Path *)om_get(&motion->paths, "disperse");
    if (existing) {
        path_free(existing);
        Path *fresh = path_new("disperse", 0.14, false, no_ease, false, 0, 0, false);
        if (!fresh) {
            return NULL;
        }
        om_insert(&motion->paths, "disperse", fresh);
    } else {
        char *out_id = NULL;
        if (motion_new_path(motion, 0.14, false, no_ease, false, 0, 0, false, "disperse", &out_id) != 0) {
            return NULL;
        }
        free(out_id);
    }
    Path *path = (Path *)om_get(&motion->paths, "disperse");
    for (int i = 0; i < 5; i++) {
        Waypoint wp;
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(path, waypoint_coords[i], NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            return NULL;
        }
        waypoint_free(&wp);
    }
    // The reference creates this path with loop_=true. The shared engine's
    // loop reactivation reuses the freed active-path string (ctx.c:380), which
    // is a use-after-free; reproduce the loop here by re-activating on
    // PATH_COMPLETE instead. Duplicate registration is expected on re-creation.
    {
        CallerKey caller;
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = "disperse";
        EventAction action;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_PATH;
        action.has_id = true;
        action.id = "disperse";
        (void)engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action);
    }
    return "disperse";
}

static void copy_cstr(char *dst, size_t cap, const char *src) {
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static int ring_add_character(Rings *st, EngineCtx *ctx, Ring *ring, CharId id, bool clockwise) {
    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
    strcpy(input_symbol, ch->input_symbol);
    bool uses_pre = ch->uses_input_preexisting_colors;

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));
    int rc = 0;

    const char *gradient_scn =
        animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false, SYNC_DISTANCE, false, no_ease,
                            "gradient", uses_pre);
    char gradient_name[64];
    copy_cstr(gradient_name, sizeof(gradient_name), gradient_scn);
    Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, gradient_name);
    if (dynamic) {
        ColorPair colors;
        memset(&colors, 0, sizeof(colors));
        if ((size_t)id < st->map_len && st->final_present[id]) {
            colors = st->final_colors[id];
        }
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors = colors;
        if (scene_add_frame(scene, input_symbol, 1, &vp) != 0) {
            rc = -1;
        }
    } else {
        Color final_fg;
        memset(&final_fg, 0, sizeof(final_fg));
        if ((size_t)id < st->map_len && st->final_present[id]) {
            final_fg = st->final_colors[id].fg;
        }
        Gradient char_gradient;
        if (gradient_pair(final_fg, ring->ring_color, 8, &char_gradient) != 0) {
            rc = -1;
        } else {
            const char *syms[1] = {input_symbol};
            if (scene_apply_gradient_to_symbols(scene, syms, 1, 3, &char_gradient, NULL) != 0) {
                rc = -1;
            }
            gradient_free(&char_gradient);
        }
    }

    char **ring_paths = NULL;
    size_t ring_paths_len = 0;
    size_t ring_paths_cap = 0;
    if (rc == 0) {
        size_t character_starting_index = ring->characters.len;
        const Coord *coords = clockwise ? ring->clockwise : ring->counter_clockwise;
        size_t coords_len = clockwise ? ring->cw_len : ring->cc_len;
        Coord *rotated = malloc((coords_len ? coords_len : 1) * sizeof(Coord));
        for (size_t k = 0; k < coords_len; k++) {
            rotated[k] = coords[(character_starting_index + k) % coords_len];
        }
        for (size_t k = 0; k < coords_len && rc == 0; k++) {
            char idbuf[32];
            snprintf(idbuf, sizeof(idbuf), "%zu", ring_paths_len);
            char *path_id = NULL;
            if (motion_new_path(&ctx->terminal.arena.items[id].motion, ring->rotation_speed, false, no_ease, false,
                                0, 0, false, idbuf, &path_id) != 0) {
                rc = -1;
                break;
            }
            Path *path = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, path_id);
            char waypoint_id[32];
            snprintf(waypoint_id, sizeof(waypoint_id), "%zu", path->waypoints_len);
            Waypoint wp;
            memset(&wp, 0, sizeof(wp));
            if (path_new_waypoint(path, rotated[k], NULL, 0, waypoint_id, &wp) != 0) {
                waypoint_free(&wp);
                free(path_id);
                rc = -1;
                break;
            }
            waypoint_free(&wp);
            if (ring_paths_len == ring_paths_cap) {
                size_t cap = ring_paths_cap ? ring_paths_cap * 2 : 8;
                ring_paths = realloc(ring_paths, cap * sizeof(char *));
                ring_paths_cap = cap;
            }
            ring_paths[ring_paths_len++] = path_id;
        }
        free(rotated);
    }

    if (rc == 0) {
        ring_map_set(&ring->last_path, id, ring_paths[0]);
    }

    if (rc == 0) {
        const char *disperse_scn =
            animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false, SYNC_DISTANCE, false,
                                no_ease, "disperse", uses_pre);
        char disperse_name[64];
        copy_cstr(disperse_name, sizeof(disperse_name), disperse_scn);
        scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, disperse_name);
        if (dynamic) {
            ColorPair colors;
            memset(&colors, 0, sizeof(colors));
            if ((size_t)id < st->map_len && st->final_present[id]) {
                colors = st->final_colors[id];
            }
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors = colors;
            if (scene_add_frame(scene, input_symbol, 1, &vp) != 0) {
                rc = -1;
            }
        } else {
            Color final_fg;
            memset(&final_fg, 0, sizeof(final_fg));
            if ((size_t)id < st->map_len && st->final_present[id]) {
                final_fg = st->final_colors[id].fg;
            }
            Gradient disperse_gradient;
            if (gradient_pair(ring->ring_color, final_fg, 8, &disperse_gradient) != 0) {
                rc = -1;
            } else {
                const char *syms[1] = {input_symbol};
                if (scene_apply_gradient_to_symbols(scene, syms, 1, 10, &disperse_gradient, NULL) != 0) {
                    rc = -1;
                }
                gradient_free(&disperse_gradient);
            }
        }
    }

    if (rc == 0) {
        if (engine_chain_paths(ctx, id, (const char *const *)ring_paths, ring_paths_len, true) != 0) {
            rc = -1;
        }
    }

    if (rc == 0) {
        idvec_push(&ring->characters, id);
    }

    for (size_t i = 0; i < ring_paths_len; i++) {
        free(ring_paths[i]);
    }
    free(ring_paths);
    free(input_symbol);
    return rc;
}

static int ring_disperse(Rings *st, EngineCtx *ctx, Effect *self, Ring *ring) {
    (void)st;
    RingIdVec characters;
    idvec_clone(&characters, &ring->characters);
    int rc = 0;
    for (size_t i = 0; i < characters.len && rc == 0; i++) {
        CharId id = characters.items[i];
        Motion *motion = &ctx->terminal.arena.items[id].motion;
        const char *last = motion->active_path ? motion->active_path : "0";
        ring_map_set(&ring->last_path, id, last);
        Coord current_coord = motion->current_coord;
        const char *disperse_path = ring_make_disperse_waypoints(ctx, ring, id, current_coord);
        if (!disperse_path) {
            rc = -1;
            break;
        }
        engine_activate_path(ctx, self, id, disperse_path);
        engine_activate_scene(ctx, self, id, "disperse");
    }
    idvec_free(&characters);
    return rc;
}

static int ring_spin(Rings *st, EngineCtx *ctx, Effect *self, Ring *ring) {
    (void)st;
    RingIdVec characters;
    idvec_clone(&characters, &ring->characters);
    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));
    int rc = 0;
    for (size_t i = 0; i < characters.len && rc == 0; i++) {
        CharId id = characters.items[i];
        const char *last_ring_path = ring_map_get(&ring->last_path, id);
        if (!last_ring_path) {
            last_ring_path = "0";
        }
        Path *last_path = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, last_ring_path);
        if (!last_path || last_path->waypoints_len == 0) {
            rc = -1;
            break;
        }
        Coord first_waypoint_coord = last_path->waypoints[0].coord;
        char *condense_path = NULL;
        if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.1, false, no_ease, false, 0, 0, false, "",
                            &condense_path) != 0) {
            rc = -1;
            break;
        }
        Path *condense = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, condense_path);
        Waypoint wp;
        memset(&wp, 0, sizeof(wp));
        if (path_new_waypoint(condense, first_waypoint_coord, NULL, 0, "", &wp) != 0) {
            waypoint_free(&wp);
            free(condense_path);
            rc = -1;
            break;
        }
        waypoint_free(&wp);

        CallerKey caller;
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_PATH;
        caller.id = (char *)condense_path;
        EventAction action;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_PATH;
        action.has_id = true;
        action.id = (char *)last_ring_path;
        if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
            free(condense_path);
            rc = -1;
            break;
        }
        engine_activate_path(ctx, self, id, condense_path);
        engine_activate_scene(ctx, self, id, "gradient");
        free(condense_path);
    }
    idvec_free(&characters);
    return rc;
}

static int rings_build(Effect *self, EngineCtx *ctx) {
    Rings *st = self->state;
    RingsConfig *cfg = &st->config;
    int rc = 0;

    int64_t min_dim = ctx->terminal.canvas.top < ctx->terminal.canvas.right ? ctx->terminal.canvas.top
                                                                           : ctx->terminal.canvas.right;
    int64_t ring_gap = py_round_half_even((double)min_dim * cfg->ring_gap);
    if (ring_gap < 1) {
        ring_gap = 1;
    }

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

    st->map_len = ctx->terminal.arena.len;
    st->final_colors = calloc(st->map_len ? st->map_len : 1, sizeof(ColorPair));
    st->final_present = calloc(st->map_len ? st->map_len : 1, sizeof(bool));

    size_t characters_len = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, filter,
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &characters_len);

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    RingIdVec pending;
    memset(&pending, 0, sizeof(pending));
    for (size_t i = 0; i < characters_len && rc == 0; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        Coord input_coord = ch->input_coord;
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);
        bool uses_pre = ch->uses_input_preexisting_colors;
        ColorPair final_colors;
        memset(&final_colors, 0, sizeof(final_colors));
        if (dynamic) {
            final_colors.has_fg = ch->animation.has_input_fg;
            final_colors.fg = ch->animation.input_fg_color;
            final_colors.has_bg = ch->animation.has_input_bg;
            final_colors.bg = ch->animation.input_bg_color;
        } else {
            const Color *mapped = coordcolormap_get(&final_gradient_mapping, input_coord);
            final_colors.has_fg = true;
            if (mapped) {
                final_colors.fg = *mapped;
            }
        }
        if ((size_t)id < st->map_len) {
            st->final_colors[id] = final_colors;
            st->final_present[id] = true;
        }

        const char *start_scn = animation_new_scene(&ctx->terminal.arena.items[id].animation, false, false,
                                                    SYNC_DISTANCE, false, no_ease, "", uses_pre);
        char start_name[64];
        copy_cstr(start_name, sizeof(start_name), start_scn);
        Scene *scene = (Scene *)om_get(&ctx->terminal.arena.items[id].animation.scenes, start_name);
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors = final_colors;
        if (scene_add_frame(scene, input_symbol, 1, &vp) != 0) {
            rc = -1;
        }

        if (rc == 0) {
            char *home_path = NULL;
            if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.8, true, easing_named(EASE_OUT_QUAD),
                                false, 0, 0, false, "home", &home_path) != 0) {
                rc = -1;
            } else {
                Path *path = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, home_path);
                Waypoint wp;
                memset(&wp, 0, sizeof(wp));
                if (path_new_waypoint(path, input_coord, NULL, 0, "", &wp) != 0) {
                    waypoint_free(&wp);
                    rc = -1;
                } else {
                    waypoint_free(&wp);
                }
                free(home_path);
            }
        }

        if (rc == 0) {
            engine_activate_scene(ctx, self, id, start_name);
            terminal_set_character_visibility(&ctx->terminal, id, true);
            idvec_push(&pending, id);
        }
        free(input_symbol);
    }
    free(characters);

    if (rc != 0) {
        idvec_free(&pending);
        coordcolormap_free(&final_gradient_mapping);
        gradient_free(&final_gradient);
        return -1;
    }

    rng_shuffle(&ctx->rng, pending.items, pending.len, sizeof(CharId));

    Coord center = ctx->terminal.canvas.center;
    int64_t radius_limit =
        ctx->terminal.canvas.right > ctx->terminal.canvas.top ? ctx->terminal.canvas.right : ctx->terminal.canvas.top;
    int64_t radius = 1;
    while (radius < radius_limit) {
        CoordVec ring_coords = find_coords_on_circle(center, radius, 7 * radius, true);
        size_t in_canvas_count = 0;
        for (size_t i = 0; i < ring_coords.len; i++) {
            if (canvas_coord_is_in_canvas(&ctx->terminal.canvas, ring_coords.items[i])) {
                in_canvas_count++;
            }
        }
        if (ring_coords.len == 0 || (double)in_canvas_count / (double)ring_coords.len < 0.25) {
            coordvec_free(&ring_coords);
            break;
        }
        Color ring_color = cfg->ring_colors.items[st->rings_len % cfg->ring_colors.len];
        Ring ring = ring_new(ctx, cfg, radius, center, ring_coords, ring_gap, ring_color);
        if (st->rings_len == st->rings_cap) {
            size_t cap = st->rings_cap ? st->rings_cap * 2 : 8;
            st->rings = realloc(st->rings, cap * sizeof(Ring));
            st->rings_cap = cap;
        }
        st->rings[st->rings_len++] = ring;
        radius += ring_gap;
    }

    bool *ring_chars = calloc(st->map_len ? st->map_len : 1, sizeof(bool));
    size_t pending_index = 0;
    for (size_t ring_count = 0; ring_count < st->rings_len && rc == 0; ring_count++) {
        Ring *ring = &st->rings[ring_count];
        for (size_t k = 0; k < ring->cc_len; k++) {
            if (pending_index < pending.len) {
                CharId next_character = pending.items[pending_index++];
                bool clockwise = ring_count % 2 == 1;
                if (ring_add_character(st, ctx, ring, next_character, clockwise) != 0) {
                    rc = -1;
                    break;
                }
                if ((size_t)next_character < st->map_len) {
                    ring_chars[next_character] = true;
                }
            }
        }
    }
    idvec_free(&pending);

    if (rc == 0) {
        size_t external_len = 0;
        CharId *external = terminal_get_characters(&ctx->terminal, &ctx->rng, filter,
                                                   CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &external_len);
        for (size_t i = 0; i < external_len && rc == 0; i++) {
            CharId id = external[i];
            if ((size_t)id < st->map_len && ring_chars[id]) {
                continue;
            }
            Coord external_coord = canvas_random_coord(&ctx->terminal.canvas, &ctx->rng, true, false);
            char *external_path = NULL;
            if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.8, true, easing_named(EASE_OUT_SINE),
                                false, 0, 0, false, "external", &external_path) != 0) {
                rc = -1;
                break;
            }
            Path *path = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, external_path);
            Waypoint wp;
            memset(&wp, 0, sizeof(wp));
            if (path_new_waypoint(path, external_coord, NULL, 0, "", &wp) != 0) {
                waypoint_free(&wp);
                free(external_path);
                rc = -1;
                break;
            }
            waypoint_free(&wp);
            free(external_path);
            idvec_push(&st->non_ring_chars, id);

            CallerKey caller;
            memset(&caller, 0, sizeof(caller));
            caller.kind = CALLER_PATH;
            caller.id = "external";
            EventAction action;
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_CALLBACK;
            action.cb.id = CB_SET_INVISIBLE;
            action.cb.args = NULL;
            action.cb.args_len = 0;
            if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
                rc = -1;
                break;
            }
        }
        free(external);
    }
    free(ring_chars);

    coordcolormap_free(&final_gradient_mapping);
    gradient_free(&final_gradient);
    if (rc != 0) {
        return -1;
    }

    st->phase = RINGS_START;
    st->initial_disperse_complete = false;
    st->spin_time_remaining = cfg->spin_duration;
    st->disperse_time_remaining = cfg->disperse_duration;
    st->cycles_remaining = cfg->spin_disperse_cycles;
    st->initial_phase_time_remaining = 100;
    return 0;
}

static const char *rings_next_frame(Effect *self, EngineCtx *ctx) {
    Rings *st = self->state;
    RingsConfig *cfg = &st->config;
    if (st->phase == RINGS_COMPLETE) {
        return NULL;
    }
    switch (st->phase) {
        case RINGS_START:
            if (st->initial_phase_time_remaining == 0) {
                st->phase = RINGS_DISPERSE;
            } else {
                st->initial_phase_time_remaining -= 1;
            }
            break;
        case RINGS_DISPERSE:
            if (!st->initial_disperse_complete) {
                st->initial_disperse_complete = true;
                for (size_t ri = 0; ri < st->rings_len; ri++) {
                    Ring *ring = &st->rings[ri];
                    RingIdVec characters;
                    idvec_clone(&characters, &ring->characters);
                    for (size_t ci = 0; ci < characters.len; ci++) {
                        CharId id = characters.items[ci];
                        Path *zero = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, "0");
                        Coord ring_start_coord = zero->waypoints[0].coord;
                        const char *disperse_path =
                            ring_make_disperse_waypoints(ctx, ring, id, ring_start_coord);
                        if (!disperse_path) {
                            idvec_free(&characters);
                            return NULL;
                        }
                        Path *disperse = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, disperse_path);
                        Coord disperse_first_coord = disperse->waypoints[0].coord;
                        char *initial_path = NULL;
                        if (motion_new_path(&ctx->terminal.arena.items[id].motion, 0.3, true,
                                            easing_named(EASE_OUT_CUBIC), false, 0, 0, false, "",
                                            &initial_path) != 0) {
                            idvec_free(&characters);
                            return NULL;
                        }
                        Path *initial = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, initial_path);
                        Waypoint wp;
                        memset(&wp, 0, sizeof(wp));
                        if (path_new_waypoint(initial, disperse_first_coord, NULL, 0, "", &wp) != 0) {
                            waypoint_free(&wp);
                            free(initial_path);
                            idvec_free(&characters);
                            return NULL;
                        }
                        waypoint_free(&wp);

                        CallerKey caller;
                        memset(&caller, 0, sizeof(caller));
                        caller.kind = CALLER_PATH;
                        caller.id = initial_path;
                        EventAction action;
                        memset(&action, 0, sizeof(action));
                        action.kind = ACTION_ACTIVATE_PATH;
                        action.has_id = true;
                        action.id = (char *)disperse_path;
                        if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
                            free(initial_path);
                            idvec_free(&characters);
                            return NULL;
                        }
                        engine_activate_scene(ctx, self, id, "disperse");
                        engine_activate_path(ctx, self, id, initial_path);
                        ac_insert(&ctx->active_characters, id);
                        free(initial_path);
                    }
                    idvec_free(&characters);
                }
                for (size_t i = 0; i < st->non_ring_chars.len; i++) {
                    CharId id = st->non_ring_chars.items[i];
                    engine_activate_path(ctx, self, id, "external");
                    ac_insert(&ctx->active_characters, id);
                }
            } else if (st->disperse_time_remaining == 0) {
                st->phase = RINGS_SPIN;
                st->cycles_remaining -= 1;
                st->spin_time_remaining = cfg->spin_duration;
                for (size_t ri = 0; ri < st->rings_len; ri++) {
                    if (ring_spin(st, ctx, self, &st->rings[ri]) != 0) {
                        return NULL;
                    }
                }
            } else {
                st->disperse_time_remaining -= 1;
            }
            break;
        case RINGS_SPIN:
            if (st->spin_time_remaining == 0) {
                if (st->cycles_remaining == 0) {
                    st->phase = RINGS_FINAL;
                    size_t characters_len = 0;
                    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng,
                                                                 character_filter_default(),
                                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &characters_len);
                    for (size_t i = 0; i < characters_len; i++) {
                        CharId id = characters[i];
                        terminal_set_character_visibility(&ctx->terminal, id, true);
                        engine_activate_path(ctx, self, id, "home");
                        ac_insert(&ctx->active_characters, id);
                        if (om_contains(&ctx->terminal.arena.items[id].motion.paths, "external")) {
                            continue;
                        }
                        engine_activate_scene(ctx, self, id, "disperse");
                    }
                    free(characters);
                } else {
                    st->disperse_time_remaining = cfg->disperse_duration;
                    for (size_t ri = 0; ri < st->rings_len; ri++) {
                        if (ring_disperse(st, ctx, self, &st->rings[ri]) != 0) {
                            return NULL;
                        }
                    }
                    st->phase = RINGS_DISPERSE;
                }
            } else {
                st->spin_time_remaining -= 1;
            }
            break;
        case RINGS_FINAL:
            if (ac_is_empty(&ctx->active_characters)) {
                st->phase = RINGS_COMPLETE;
            }
            break;
        case RINGS_COMPLETE:
            break;
    }
    engine_update(ctx, self);
    return engine_frame(ctx);
}

static void rings_callback(Effect *self, EngineCtx *ctx, CharId character, const EffectCallback *cb) {
    (void)self;
    if (cb->id == CB_SET_INVISIBLE) {
        terminal_set_character_visibility(&ctx->terminal, character, false);
    }
}

static void rings_destroy(Effect *self) {
    Rings *st = self->state;
    if (!st) {
        return;
    }
    for (size_t i = 0; i < st->rings_len; i++) {
        ring_free(&st->rings[i]);
    }
    free(st->rings);
    idvec_free(&st->non_ring_chars);
    free(st->final_colors);
    free(st->final_present);
    free(st);
    free(self);
}

static const EffectOps RINGS_OPS = {rings_build, rings_next_frame, rings_destroy, rings_callback};

Effect *rings_make(const void *cfg) {
    Rings *st = calloc(1, sizeof(Rings));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const RingsConfig *)cfg;
    st->phase = RINGS_START;
    effect->ops = &RINGS_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec rings_specs[] = {
    EF_SPEC("ring-colors", 0, EF_COLOR_LIST, offsetof(RingsConfig, ring_colors)),
    EF_SPEC("ring-gap", 0, EF_FLOAT_POS, offsetof(RingsConfig, ring_gap)),
    EF_SPEC("spin-duration", 0, EF_POS_INT, offsetof(RingsConfig, spin_duration)),
    EF_SPEC("spin-speed", 0, EF_FLOAT_RANGE, offsetof(RingsConfig, spin_speed)),
    EF_SPEC("disperse-duration", 0, EF_POS_INT, offsetof(RingsConfig, disperse_duration)),
    EF_SPEC("spin-disperse-cycles", 0, EF_POS_INT, offsetof(RingsConfig, spin_disperse_cycles)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(RingsConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(RingsConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(RingsConfig, final_gradient_direction)),
};

const EffectEntry rings_entry = {
    "rings",
    rings_specs,
    sizeof(rings_specs) / sizeof(rings_specs[0]),
    sizeof(RingsConfig),
    rings_config_defaults,
    rings_free_config,
    rings_make,
};
