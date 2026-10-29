#include "effects/burnout.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "effects/originals.h"
#include "effects/registry.h"
#include "engine/terminal.h"

// A character keeps its own symbol for this share of its burn, flashing
// through the hottest ember colours before it decays through the symbols.
#define IGNITE_SHARE 0.15
// Sparks rise this many rows per frame.
#define SPARK_RISE 0.083

typedef struct {
    BurnoutConfig config;
    OrigCast cast;
    int64_t *ignite;   // frame each character catches fire
    CharId *spark;     // CHAR_ID_NONE when the character throws no spark
    int64_t frame;
    int64_t last_frame;
} Burnout;

void burnout_config_defaults(void *cfg_ptr) {
    BurnoutConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->hold_frames = 36;
    cfg->burn_direction = CG_COLUMN_LEFT_TO_RIGHT;
    cfg->burn_spread = 72;
    cfg->burn_jitter = 72;
    cfg->burn_frames = 33;
    orig_push_symbol(&cfg->burn_symbols, "▓");
    orig_push_symbol(&cfg->burn_symbols, "▒");
    orig_push_symbol(&cfg->burn_symbols, "░");
    orig_push_symbol(&cfg->burn_symbols, "·");
    orig_push_color(&cfg->ember_colors, "ffffff");
    orig_push_color(&cfg->ember_colors, "ffd88a");
    orig_push_color(&cfg->ember_colors, "ff7a00");
    orig_push_color(&cfg->ember_colors, "7a1a00");
    cfg->spark_ratio = 0.35;
    cfg->spark_frames = 54;
    orig_push_color(&cfg->final_gradient_stops, "82fb9c");
    orig_push_color(&cfg->final_gradient_stops, "7cf8f7");
    orig_push_color(&cfg->final_gradient_stops, "8a7cf8");
    orig_push_int(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_HORIZONTAL;
}

void burnout_free_config(void *cfg_ptr) {
    BurnoutConfig *cfg = cfg_ptr;
    orig_free_symbols(&cfg->burn_symbols);
    free(cfg->ember_colors.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int burnout_build(Effect *self, EngineCtx *ctx) {
    Burnout *st = self->state;
    BurnoutConfig *cfg = &st->config;
    if (orig_cast_build(ctx, &cfg->final_gradient_stops, &cfg->final_gradient_steps,
                        cfg->final_gradient_direction, &st->cast) != 0) {
        return -1;
    }
    size_t n = st->cast.len;
    st->ignite = calloc(n ? n : 1, sizeof(int64_t));
    st->spark = calloc(n ? n : 1, sizeof(CharId));
    size_t *index_of = calloc(ctx->terminal.arena.len + 1, sizeof(size_t));
    if (!st->ignite || !st->spark || !index_of) {
        free(index_of);
        return -1;
    }
    for (size_t i = 0; i < n; i++) {
        index_of[st->cast.ids[i]] = i;
    }
    // Ignition drifts across the text in burn-direction order, each character
    // late by a random share of the jitter.
    CharIdGrouping groups =
        terminal_get_characters_grouped(&ctx->terminal, character_filter_default(), cfg->burn_direction);
    double span = groups.len > 1 ? (double)(groups.len - 1) : 1.0;
    for (size_t g = 0; g < groups.len; g++) {
        int64_t base = cfg->hold_frames + (int64_t)floor((double)g / span * (double)cfg->burn_spread + 0.5);
        for (size_t k = 0; k < groups.buckets[g].len; k++) {
            size_t i = index_of[groups.buckets[g].items[k]];
            st->ignite[i] = base + rng_randint(&ctx->rng, 0, cfg->burn_jitter);
        }
    }
    charidgrouping_free(&groups);
    free(index_of);

    st->last_frame = 0;
    for (size_t i = 0; i < n; i++) {
        CharId id = st->cast.ids[i];
        const EffectCharacter *ch = &ctx->terminal.arena.items[id];
        terminal_set_character_visibility(&ctx->terminal, id, true);
        orig_look(ctx, id, ch->input_symbol, st->cast.has_final[i] ? &st->cast.final[i] : NULL, NULL);
        st->spark[i] = CHAR_ID_NONE;
        int64_t end = st->ignite[i] + cfg->burn_frames;
        if (rng_random(&ctx->rng) < cfg->spark_ratio && cfg->spark_frames > 0) {
            st->spark[i] = terminal_add_character(&ctx->terminal, "*", ch->input_coord);
            orig_layer(ctx, st->spark[i], 1);
            int64_t spark_end = st->ignite[i] + cfg->spark_frames + 6;
            end = end > spark_end ? end : spark_end;
        }
        st->last_frame = end > st->last_frame ? end : st->last_frame;
    }
    return 0;
}

static void burn_char(EngineCtx *ctx, Burnout *st, size_t i, int64_t age) {
    const BurnoutConfig *cfg = &st->config;
    CharId id = st->cast.ids[i];
    const EffectCharacter *ch = &ctx->terminal.arena.items[id];
    if (age >= cfg->burn_frames) {
        orig_hide(ctx, id);
        return;
    }
    double p = (double)age / (double)cfg->burn_frames;
    const char *symbol = ch->input_symbol;
    if (p >= IGNITE_SHARE && cfg->burn_symbols.len) {
        size_t n = cfg->burn_symbols.len;
        size_t s = (size_t)((p - IGNITE_SHARE) / (1.0 - IGNITE_SHARE) * (double)n);
        symbol = cfg->burn_symbols.items[s < n ? s : n - 1];
    }
    Color fg = orig_ramp(cfg->ember_colors.items, cfg->ember_colors.len, p);
    Color hot = orig_ramp(cfg->ember_colors.items, cfg->ember_colors.len, 0.66);
    Color bg = orig_mix(&ctx->terminal.config.terminal_background_color, &hot, (1.0 - p) * 0.35);
    orig_look(ctx, id, symbol, &fg, p < 0.9 ? &bg : NULL);
}

static void move_spark(EngineCtx *ctx, Burnout *st, size_t i, int64_t age) {
    const BurnoutConfig *cfg = &st->config;
    CharId sid = st->spark[i];
    int64_t sa = age - 6;
    if (sa < 0 || sa >= cfg->spark_frames) {
        orig_hide(ctx, sid);
        return;
    }
    const EffectCharacter *ch = &ctx->terminal.arena.items[st->cast.ids[i]];
    double fs = (double)sa;
    double x = (double)ch->input_coord.column + sin(fs * 0.1 + (double)i) * 0.8;
    double y = (double)ch->input_coord.row + fs * SPARK_RISE;
    double p = fs / (double)cfg->spark_frames;
    Color c = orig_ramp(cfg->ember_colors.items, cfg->ember_colors.len, 0.3 + 0.7 * p);
    orig_look(ctx, sid, p < 0.33 ? "*" : "·", &c, NULL);
    orig_place(ctx, sid, coord_new((int64_t)floor(x + 0.5), (int64_t)floor(y + 0.5)));
}

static const char *burnout_next_frame(Effect *self, EngineCtx *ctx) {
    Burnout *st = self->state;
    int64_t f = st->frame;
    if (f > st->last_frame) {
        return NULL;
    }
    for (size_t i = 0; i < st->cast.len; i++) {
        int64_t age = f - st->ignite[i];
        if (age < 0) {
            continue;
        }
        burn_char(ctx, st, i, age);
        if (st->spark[i] != CHAR_ID_NONE) {
            move_spark(ctx, st, i, age);
        }
    }
    st->frame++;
    return engine_frame(ctx);
}

static void burnout_destroy(Effect *self) {
    Burnout *st = self->state;
    if (!st) {
        return;
    }
    orig_cast_free(&st->cast);
    free(st->ignite);
    free(st->spark);
    free(st);
    free(self);
}

static const EffectOps BURNOUT_OPS = {burnout_build, burnout_next_frame, burnout_destroy, NULL};

Effect *burnout_make(const void *cfg) {
    Burnout *st = calloc(1, sizeof(Burnout));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const BurnoutConfig *)cfg;
    effect->ops = &BURNOUT_OPS;
    effect->state = st;
    return effect;
}

const EffectEntry burnout_entry = {
    "burnout",
    (const EffOptSpec[]){
        EF_SPEC("hold-frames", 0, EF_NONNEG_INT, offsetof(BurnoutConfig, hold_frames)),
        EF_SPEC("burn-direction", 0, EF_CHAR_GROUP, offsetof(BurnoutConfig, burn_direction)),
        EF_SPEC("burn-spread", 0, EF_NONNEG_INT, offsetof(BurnoutConfig, burn_spread)),
        EF_SPEC("burn-jitter", 0, EF_NONNEG_INT, offsetof(BurnoutConfig, burn_jitter)),
        EF_SPEC("burn-frames", 0, EF_POS_INT, offsetof(BurnoutConfig, burn_frames)),
        EF_SPEC("burn-symbols", 0, EF_STRING_LIST, offsetof(BurnoutConfig, burn_symbols)),
        EF_SPEC("ember-colors", 0, EF_COLOR_LIST, offsetof(BurnoutConfig, ember_colors)),
        EF_SPEC("spark-ratio", 0, EF_RATIO_NONNEG, offsetof(BurnoutConfig, spark_ratio)),
        EF_SPEC("spark-frames", 0, EF_NONNEG_INT, offsetof(BurnoutConfig, spark_frames)),
        EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(BurnoutConfig, final_gradient_stops)),
        EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(BurnoutConfig, final_gradient_steps)),
        EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(BurnoutConfig, final_gradient_direction)),
    },
    12,
    sizeof(BurnoutConfig),
    burnout_config_defaults,
    burnout_free_config,
    burnout_make,
};
