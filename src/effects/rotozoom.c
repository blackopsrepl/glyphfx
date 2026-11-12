#include "effects/rotozoom.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "effects/originals.h"
#include "effects/registry.h"
#include "engine/terminal.h"

// The demoscene rotozoomer, with the text as its texture.
//
// The text is tiled endlessly across a plane. Every canvas cell maps back
// through the inverse rotation and zoom onto that plane and shows the
// character it lands on, nearest neighbour, in that character's own colour:
// texture mapping from screen to texture, as the real effect does, so the
// picture has no holes or collisions at any angle. The plane spins, its zoom
// breathes out and in, and the view pans in a slow circle; all three ease
// away together and the plane locks to rest exactly on the text.

typedef struct {
    RotozoomConfig config;
    OrigCast cast;
    int64_t *grid;    // text cell -> cast index + 1, 0 for a gap in the text
    int64_t tl, tb, tw, th;  // text bounds
    int64_t px, py;   // tile period: the text and the gap around it
    double pcx, pcy;  // pivot: the middle of the text
    double pan_phase;
    CharId *disp;     // the character living at each canvas cell
    int64_t left, bottom, w, h;
    int64_t frame;
} Rotozoom;

void rotozoom_config_defaults(void *cfg_ptr) {
    RotozoomConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->roto_frames = 180;
    cfg->turns = 1.5;
    cfg->zoom_depth = 0.4;
    cfg->pan_radius = 0.2;
    orig_push_color(&cfg->final_gradient_stops, "82fb9c");
    orig_push_color(&cfg->final_gradient_stops, "7cf8f7");
    orig_push_color(&cfg->final_gradient_stops, "8a7cf8");
    orig_push_int(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_HORIZONTAL;
}

void rotozoom_free_config(void *cfg_ptr) {
    RotozoomConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

static int rotozoom_build(Effect *self, EngineCtx *ctx) {
    Rotozoom *st = self->state;
    RotozoomConfig *cfg = &st->config;
    if (orig_cast_build(ctx, &cfg->final_gradient_stops, &cfg->final_gradient_steps,
                        cfg->final_gradient_direction, &st->cast) != 0) {
        return -1;
    }
    const Canvas *cv = &ctx->terminal.canvas;
    st->left = cv->left;
    st->bottom = cv->bottom;
    st->w = cv->right - cv->left + 1;
    st->h = cv->top - cv->bottom + 1;
    st->pan_phase = rng_random(&ctx->rng) * 2.0 * M_PI;

    // The texture: the text's own bounding box, over every input character
    // (some may lie outside the canvas).
    int64_t l = INT64_MAX, r = INT64_MIN, b = INT64_MAX, t = INT64_MIN;
    for (size_t i = 0; i < st->cast.len; i++) {
        Coord c = ctx->terminal.arena.items[st->cast.ids[i]].input_coord;
        l = c.column < l ? c.column : l;
        r = c.column > r ? c.column : r;
        b = c.row < b ? c.row : b;
        t = c.row > t ? c.row : t;
    }
    if (st->cast.len == 0) {
        l = r = st->left;
        b = t = st->bottom;
    }
    st->tl = l;
    st->tb = b;
    st->tw = r - l + 1;
    st->th = t - b + 1;
    st->px = st->tw + (st->tw / 6 > 2 ? st->tw / 6 : 2);
    st->py = st->th + (st->th / 3 > 1 ? st->th / 3 : 1);
    st->pcx = (double)(l + r) / 2.0;
    st->pcy = (double)(b + t) / 2.0;
    st->grid = calloc((size_t)(st->tw * st->th), sizeof(int64_t));
    st->disp = calloc((size_t)(st->w * st->h), sizeof(CharId));
    if (!st->grid || !st->disp) {
        return -1;
    }
    for (size_t i = 0; i < st->cast.len; i++) {
        Coord c = ctx->terminal.arena.items[st->cast.ids[i]].input_coord;
        st->grid[(c.row - st->tb) * st->tw + (c.column - st->tl)] = (int64_t)i + 1;
    }
    for (int64_t ry = 0; ry < st->h; ry++) {
        for (int64_t rx = 0; rx < st->w; rx++) {
            st->disp[ry * st->w + rx] =
                terminal_get_character_by_input_coord(&ctx->terminal, coord_new(st->left + rx, st->bottom + ry));
        }
    }
    return 0;
}

static const char *rotozoom_next_frame(Effect *self, EngineCtx *ctx) {
    Rotozoom *st = self->state;
    const RotozoomConfig *cfg = &st->config;
    int64_t f = st->frame;
    if (f > cfg->roto_frames) {
        return NULL;
    }
    double p = (double)f / (double)cfg->roto_frames;
    // Every motion shares one envelope that eases to rest at the last frame.
    double env = 1.0 - orig_smooth(p);
    double rest = 1.0 - p;
    double angle = cfg->turns * 2.0 * M_PI * rest * rest;
    // The zoom starts far out, swings in past full size, and settles.
    double zoom = exp(log(cfg->zoom_depth) * env * cos(2.0 * M_PI * 1.25 * p));
    double pan = cfg->pan_radius * env;
    double ox = pan * (double)st->tw * cos(st->pan_phase + 2.0 * M_PI * 0.75 * p);
    double oy = pan * (double)st->th * 2.0 * sin(st->pan_phase + 2.0 * M_PI * 0.75 * p);
    double c = cos(angle) / zoom, s = sin(angle) / zoom;
    // Near the end only the home tile remains, so the text locks in alone.
    bool tiled = env > 0.04;
    for (int64_t ry = 0; ry < st->h; ry++) {
        double y = ((double)(st->bottom + ry) - st->pcy) * 2.0;  // a cell is about twice as tall as wide
        for (int64_t rx = 0; rx < st->w; rx++) {
            CharId id = st->disp[ry * st->w + rx];
            if (id == CHAR_ID_NONE) {
                continue;
            }
            double x = (double)(st->left + rx) - st->pcx;
            double tx = st->pcx + ox + c * x + s * y;
            double ty = st->pcy + (oy + (-s * x + c * y)) / 2.0;
            int64_t ux = (int64_t)floor(tx + 0.5) - st->tl, uy = (int64_t)floor(ty + 0.5) - st->tb;
            int64_t i = floor_div(ux, st->px), j = floor_div(uy, st->py);
            int64_t lx = ux - i * st->px, ly = uy - j * st->py;
            int64_t src = (lx < st->tw && ly < st->th) ? st->grid[ly * st->tw + lx] : 0;
            if (src == 0 || (!tiled && (i != 0 || j != 0))) {
                orig_hide(ctx, id);
                continue;
            }
            size_t k = (size_t)(src - 1);
            orig_look(ctx, id, orig_input_symbol(ctx, st->cast.ids[k]),
                      st->cast.has_final[k] ? &st->cast.final[k] : NULL, NULL);
            terminal_set_character_visibility(&ctx->terminal, id, true);
        }
    }
    st->frame++;
    return engine_frame(ctx);
}

static void rotozoom_destroy(Effect *self) {
    Rotozoom *st = self->state;
    if (!st) {
        return;
    }
    orig_cast_free(&st->cast);
    free(st->grid);
    free(st->disp);
    free(st);
    free(self);
}

static const EffectOps ROTOZOOM_OPS = {rotozoom_build, rotozoom_next_frame, rotozoom_destroy, NULL};

Effect *rotozoom_make(const void *cfg) {
    Rotozoom *st = calloc(1, sizeof(Rotozoom));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const RotozoomConfig *)cfg;
    effect->ops = &ROTOZOOM_OPS;
    effect->state = st;
    return effect;
}

const EffectEntry rotozoom_entry = {
    "rotozoom",
    (const EffOptSpec[]){
        EF_SPEC("roto-frames", 0, EF_POS_INT, offsetof(RotozoomConfig, roto_frames)),
        EF_SPEC("turns", 0, EF_FLOAT_NONNEG, offsetof(RotozoomConfig, turns)),
        EF_SPEC("zoom-depth", 0, EF_RATIO_POS, offsetof(RotozoomConfig, zoom_depth)),
        EF_SPEC("pan-radius", 0, EF_FLOAT_NONNEG, offsetof(RotozoomConfig, pan_radius)),
        EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(RotozoomConfig, final_gradient_stops)),
        EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(RotozoomConfig, final_gradient_steps)),
        EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(RotozoomConfig, final_gradient_direction)),
    },
    7,
    sizeof(RotozoomConfig),
    rotozoom_config_defaults,
    rotozoom_free_config,
    rotozoom_make,
};
