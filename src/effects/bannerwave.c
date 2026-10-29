#include "effects/bannerwave.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "effects/originals.h"
#include "effects/registry.h"
#include "engine/terminal.h"

// The reveal front runs this many columns past each edge of the text, and
// characters within FRONT_GLOW columns behind it are lit.
#define FRONT_MARGIN 6.0
#define FRONT_GLOW 4.0

typedef struct {
    BannerwaveConfig config;
    OrigCast cast;
    double left;
    double right;
    int64_t frame;
    int64_t last_frame;
} Bannerwave;

void bannerwave_config_defaults(void *cfg_ptr) {
    BannerwaveConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->wave_amplitude = 1.6;
    cfg->wave_length = 63;
    cfg->wave_period = 75;
    cfg->reveal_frames = 66;
    cfg->wave_frames = 156;
    cfg->settle_frames = 90;
    color_from_hex("ffffff", &cfg->highlight_color);
    orig_push_color(&cfg->final_gradient_stops, "82fb9c");
    orig_push_color(&cfg->final_gradient_stops, "7cf8f7");
    orig_push_color(&cfg->final_gradient_stops, "8a7cf8");
    orig_push_int(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_HORIZONTAL;
}

void bannerwave_free_config(void *cfg_ptr) {
    BannerwaveConfig *cfg = cfg_ptr;
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int bannerwave_build(Effect *self, EngineCtx *ctx) {
    Bannerwave *st = self->state;
    BannerwaveConfig *cfg = &st->config;
    if (orig_cast_build(ctx, &cfg->final_gradient_stops, &cfg->final_gradient_steps,
                        cfg->final_gradient_direction, &st->cast) != 0) {
        return -1;
    }
    st->left = (double)ctx->terminal.canvas.text_left - FRONT_MARGIN;
    st->right = (double)ctx->terminal.canvas.text_right + FRONT_MARGIN;
    int64_t end = cfg->wave_frames + cfg->settle_frames;
    st->last_frame = end > cfg->reveal_frames ? end : cfg->reveal_frames;
    return 0;
}

static const char *bannerwave_next_frame(Effect *self, EngineCtx *ctx) {
    Bannerwave *st = self->state;
    const BannerwaveConfig *cfg = &st->config;
    int64_t f = st->frame;
    if (f > st->last_frame) {
        return NULL;
    }
    double t = (double)f;
    double front = st->left + orig_clamp01(t / (double)cfg->reveal_frames) * (st->right - st->left);
    if (f >= cfg->reveal_frames) {
        front = st->right + FRONT_GLOW;
    }
    // The swell rises over the first 30 frames and dies over the settle.
    double swell = orig_smooth(t / 30.0) *
                   (1.0 - orig_smooth((t - (double)cfg->wave_frames) / (double)cfg->settle_frames));
    if (f >= st->last_frame) {
        swell = 0.0;
    }
    double amp = cfg->wave_amplitude * swell;
    double k = 2.0 * M_PI / (double)cfg->wave_length;
    double w = 2.0 * M_PI / (double)cfg->wave_period;
    for (size_t i = 0; i < st->cast.len; i++) {
        CharId id = st->cast.ids[i];
        const EffectCharacter *ch = &ctx->terminal.arena.items[id];
        double x = (double)ch->input_coord.column;
        if (x > front) {
            orig_hide(ctx, id);
            continue;
        }
        double s = sin(x * k - t * w);
        int64_t dy = (int64_t)floor(amp * s + 0.5);
        double crest = (s > 0.0 ? s : 0.0) * swell;
        double near = 1.0 - (front - x) / FRONT_GLOW;
        near = near > 0.0 ? near : 0.0;
        const Color *fin = st->cast.has_final[i] ? &st->cast.final[i] : NULL;
        if (!fin || (crest <= 0.0 && near <= 0.0)) {
            orig_look(ctx, id, ch->input_symbol, fin, NULL);
        } else {
            Color lit = orig_mix(fin, &cfg->highlight_color, crest * 0.45);
            Color c = orig_mix(&lit, &cfg->highlight_color, near);
            orig_look(ctx, id, ch->input_symbol, &c, NULL);
        }
        orig_place(ctx, id, coord_new(ch->input_coord.column, ch->input_coord.row + dy));
    }
    st->frame++;
    return engine_frame(ctx);
}

static void bannerwave_destroy(Effect *self) {
    Bannerwave *st = self->state;
    if (!st) {
        return;
    }
    orig_cast_free(&st->cast);
    free(st);
    free(self);
}

static const EffectOps BANNERWAVE_OPS = {bannerwave_build, bannerwave_next_frame, bannerwave_destroy, NULL};

Effect *bannerwave_make(const void *cfg) {
    Bannerwave *st = calloc(1, sizeof(Bannerwave));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const BannerwaveConfig *)cfg;
    effect->ops = &BANNERWAVE_OPS;
    effect->state = st;
    return effect;
}

const EffectEntry bannerwave_entry = {
    "bannerwave",
    (const EffOptSpec[]){
        EF_SPEC("wave-amplitude", 0, EF_FLOAT_NONNEG, offsetof(BannerwaveConfig, wave_amplitude)),
        EF_SPEC("wave-length", 0, EF_POS_INT, offsetof(BannerwaveConfig, wave_length)),
        EF_SPEC("wave-period", 0, EF_POS_INT, offsetof(BannerwaveConfig, wave_period)),
        EF_SPEC("reveal-frames", 0, EF_POS_INT, offsetof(BannerwaveConfig, reveal_frames)),
        EF_SPEC("wave-frames", 0, EF_NONNEG_INT, offsetof(BannerwaveConfig, wave_frames)),
        EF_SPEC("settle-frames", 0, EF_POS_INT, offsetof(BannerwaveConfig, settle_frames)),
        EF_SPEC("highlight-color", 0, EF_COLOR, offsetof(BannerwaveConfig, highlight_color)),
        EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(BannerwaveConfig, final_gradient_stops)),
        EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(BannerwaveConfig, final_gradient_steps)),
        EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(BannerwaveConfig, final_gradient_direction)),
    },
    10,
    sizeof(BannerwaveConfig),
    bannerwave_config_defaults,
    bannerwave_free_config,
    bannerwave_make,
};
