#include "effects/copper.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "effects/originals.h"
#include "effects/registry.h"
#include "engine/terminal.h"

typedef struct {
    CopperConfig config;
    OrigCast cast;
    CharId *fills;
    size_t n_fills;
    Color *row_color;  // per canvas row, indexed from the top
    double *row_light;
    int64_t rows;
    int64_t fade_start;
    int64_t frame;
    int64_t last_frame;
} Copper;

void copper_config_defaults(void *cfg_ptr) {
    CopperConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    orig_push_color(&cfg->bar_colors, "82fb9c");
    orig_push_color(&cfg->bar_colors, "7cf8f7");
    orig_push_color(&cfg->bar_colors, "8a7cf8");
    cfg->bar_height = 5;
    cfg->bounce_frames = 180;
    cfg->settle_frames = 80;
    color_from_hex("3c3f5a", &cfg->unlit_color);
    color_from_hex("e8fff0", &cfg->sweep_color);
    orig_push_color(&cfg->final_gradient_stops, "82fb9c");
    orig_push_color(&cfg->final_gradient_stops, "7cf8f7");
    orig_push_color(&cfg->final_gradient_stops, "8a7cf8");
    orig_push_int(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_HORIZONTAL;
}

void copper_free_config(void *cfg_ptr) {
    CopperConfig *cfg = cfg_ptr;
    free(cfg->bar_colors.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int copper_build(Effect *self, EngineCtx *ctx) {
    Copper *st = self->state;
    CopperConfig *cfg = &st->config;
    if (orig_cast_build(ctx, &cfg->final_gradient_stops, &cfg->final_gradient_steps,
                        cfg->final_gradient_direction, &st->cast) != 0) {
        return -1;
    }
    CharacterFilter fills = {false, true, true, false};
    st->fills = terminal_get_characters(&ctx->terminal, &ctx->rng, fills, CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT,
                                        &st->n_fills);
    st->rows = ctx->terminal.canvas.top;
    st->row_color = calloc((size_t)st->rows + 1, sizeof(Color));
    st->row_light = calloc((size_t)st->rows + 1, sizeof(double));
    if (!st->row_color || !st->row_light) {
        return -1;
    }
    // The bars fade out over the last quarter of the bounce, and the settling
    // sweep starts as they fade.
    st->fade_start = cfg->bounce_frames - cfg->bounce_frames / 4;
    int64_t fade_len = cfg->bounce_frames / 4 + 1;
    st->last_frame = st->fade_start + (cfg->settle_frames > fade_len ? cfg->settle_frames : fade_len);
    for (size_t i = 0; i < st->cast.len; i++) {
        CharId id = st->cast.ids[i];
        terminal_set_character_visibility(&ctx->terminal, id, true);
        orig_look(ctx, id, ctx->terminal.arena.items[id].input_symbol, &cfg->unlit_color, NULL);
    }
    return 0;
}

static double sweep_row(const Copper *st, int64_t f) {
    double p = orig_smooth((double)(f - st->fade_start) / (double)st->config.settle_frames);
    return -2.0 + p * ((double)st->rows + 4.0);
}

// Lights every row for frame f: the bouncing bars, then the settling sweep.
static void light_rows(Copper *st, int64_t f) {
    const CopperConfig *cfg = &st->config;
    double secs = (double)f / 60.0;
    double fade = 1.0 - orig_smooth((double)(f - st->fade_start) / (double)(cfg->bounce_frames / 4 + 1));
    double half = (double)cfg->bar_height / 2.0;
    double sweep = sweep_row(st, f);
    bool sweeping = f >= st->fade_start;
    for (int64_t r = 0; r < st->rows; r++) {
        double y = (double)r, best = 0.0;
        Color col = cfg->sweep_color;
        for (size_t k = 0; k < cfg->bar_colors.len; k++) {
            double kk = (double)k;
            double centre = (double)st->rows * (0.5 + 0.48 * sin(secs * (1.7 + 0.45 * kk) + kk * 2.1));
            double v = (1.0 - fabs(y - centre) / half) * fade;
            if (v > best) {
                best = v;
                col = cfg->bar_colors.items[k];
            }
        }
        if (sweeping) {
            double v = 1.0 - fabs(y - sweep) / 1.6;
            if (v > best) {
                best = v;
                col = cfg->sweep_color;
            }
        }
        st->row_light[r] = best > 0.0 ? best : 0.0;
        st->row_color[r] = col;
    }
}

static const char *copper_next_frame(Effect *self, EngineCtx *ctx) {
    Copper *st = self->state;
    const CopperConfig *cfg = &st->config;
    int64_t f = st->frame;
    if (f > st->last_frame) {
        return NULL;
    }
    light_rows(st, f);
    const Color *term_bg = &ctx->terminal.config.terminal_background_color;
    const Color white = color_from_rgb(255, 255, 255);
    double sweep = f >= st->fade_start ? sweep_row(st, f) : -1e9;
    for (size_t i = 0; i < st->cast.len; i++) {
        CharId id = st->cast.ids[i];
        const EffectCharacter *ch = &ctx->terminal.arena.items[id];
        int64_t r = st->rows - ch->input_coord.row;
        double v = st->row_light[r];
        const Color *fin = st->cast.has_final[i] ? &st->cast.final[i] : NULL;
        const Color *base = ((double)r < sweep) ? fin : &cfg->unlit_color;
        if (v <= 0.0) {
            orig_look(ctx, id, ch->input_symbol, base, NULL);
            continue;
        }
        Color lit = orig_mix(&st->row_color[r], &white, 0.5);
        Color fg = base ? orig_mix(base, &lit, v * 0.8) : lit;
        Color glow = orig_mix(&st->row_color[r], &white, v * 0.4);
        Color bg = orig_mix(term_bg, &glow, 0.6 * v);
        orig_look(ctx, id, ch->input_symbol, &fg, &bg);
    }
    for (size_t i = 0; i < st->n_fills; i++) {
        CharId id = st->fills[i];
        int64_t r = st->rows - ctx->terminal.arena.items[id].input_coord.row;
        double v = st->row_light[r];
        if (v <= 0.0) {
            orig_hide(ctx, id);
            continue;
        }
        Color glow = orig_mix(&st->row_color[r], &white, v * 0.4);
        Color bg = orig_mix(term_bg, &glow, 0.6 * v);
        orig_look(ctx, id, " ", NULL, &bg);
        terminal_set_character_visibility(&ctx->terminal, id, true);
    }
    st->frame++;
    return engine_frame(ctx);
}

static void copper_destroy(Effect *self) {
    Copper *st = self->state;
    if (!st) {
        return;
    }
    orig_cast_free(&st->cast);
    free(st->fills);
    free(st->row_color);
    free(st->row_light);
    free(st);
    free(self);
}

static const EffectOps COPPER_OPS = {copper_build, copper_next_frame, copper_destroy, NULL};

Effect *copper_make(const void *cfg) {
    Copper *st = calloc(1, sizeof(Copper));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const CopperConfig *)cfg;
    effect->ops = &COPPER_OPS;
    effect->state = st;
    return effect;
}

const EffectEntry copper_entry = {
    "copper",
    (const EffOptSpec[]){
        EF_SPEC("bar-colors", 0, EF_COLOR_LIST, offsetof(CopperConfig, bar_colors)),
        EF_SPEC("bar-height", 0, EF_POS_INT, offsetof(CopperConfig, bar_height)),
        EF_SPEC("bounce-frames", 0, EF_POS_INT, offsetof(CopperConfig, bounce_frames)),
        EF_SPEC("settle-frames", 0, EF_POS_INT, offsetof(CopperConfig, settle_frames)),
        EF_SPEC("unlit-color", 0, EF_COLOR, offsetof(CopperConfig, unlit_color)),
        EF_SPEC("sweep-color", 0, EF_COLOR, offsetof(CopperConfig, sweep_color)),
        EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(CopperConfig, final_gradient_stops)),
        EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(CopperConfig, final_gradient_steps)),
        EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(CopperConfig, final_gradient_direction)),
    },
    9,
    sizeof(CopperConfig),
    copper_config_defaults,
    copper_free_config,
    copper_make,
};
