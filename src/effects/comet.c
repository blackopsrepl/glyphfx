#include "effects/comet.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "effects/originals.h"
#include "effects/registry.h"
#include "engine/terminal.h"

// Trail points trail the head by this many frames each.
#define TRAIL_SPACING 1.5

typedef struct {
    CometConfig config;
    OrigCast cast;
    int64_t *launch;   // launch frame per character
    double *lift;      // arc height per character, in rows
    CharId *trail;     // trail_length added characters per input character
    unsigned char *state;  // 0 waiting, 1 flying, 2 landed and cooling, 3 settled
    Coord origin;
    int64_t top_row;
    int64_t frame;
    int64_t last_frame;
} Comet;

void comet_config_defaults(void *cfg_ptr) {
    CometConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->launch_order = CG_COLUMN_LEFT_TO_RIGHT;
    cfg->launch_delay = 2;
    cfg->launch_jitter = 10;
    cfg->flight_frames = 50;
    easing_parse("in_out_sine", &cfg->flight_ease);
    cfg->arc_height.start = 3;
    cfg->arc_height.end = 8;
    cfg->trail_length = 10;
    orig_push_symbol(&cfg->trail_symbols, "▓");
    orig_push_symbol(&cfg->trail_symbols, "▒");
    orig_push_symbol(&cfg->trail_symbols, "░");
    orig_push_symbol(&cfg->trail_symbols, "·");
    color_from_hex("ffffff", &cfg->comet_color);
    cfg->cooldown_frames = 20;
    orig_push_color(&cfg->final_gradient_stops, "82fb9c");
    orig_push_color(&cfg->final_gradient_stops, "7cf8f7");
    orig_push_color(&cfg->final_gradient_stops, "8a7cf8");
    orig_push_int(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_HORIZONTAL;
}

void comet_free_config(void *cfg_ptr) {
    CometConfig *cfg = cfg_ptr;
    orig_free_symbols(&cfg->trail_symbols);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int comet_build(Effect *self, EngineCtx *ctx) {
    Comet *st = self->state;
    CometConfig *cfg = &st->config;
    if (orig_cast_build(ctx, &cfg->final_gradient_stops, &cfg->final_gradient_steps,
                        cfg->final_gradient_direction, &st->cast) != 0) {
        return -1;
    }
    size_t n = st->cast.len;
    st->launch = calloc(n ? n : 1, sizeof(int64_t));
    st->lift = calloc(n ? n : 1, sizeof(double));
    st->state = calloc(n ? n : 1, 1);
    st->trail = calloc(n * (size_t)cfg->trail_length + 1, sizeof(CharId));
    if (!st->launch || !st->lift || !st->state || !st->trail) {
        return -1;
    }
    // Launch frame: the group's slot plus a per-character jitter.
    size_t *index_of = calloc(ctx->terminal.arena.len + 1, sizeof(size_t));
    for (size_t i = 0; i < n; i++) {
        index_of[st->cast.ids[i]] = i;
    }
    CharIdGrouping groups =
        terminal_get_characters_grouped(&ctx->terminal, character_filter_default(), cfg->launch_order);
    for (size_t g = 0; g < groups.len; g++) {
        for (size_t k = 0; k < groups.buckets[g].len; k++) {
            size_t i = index_of[groups.buckets[g].items[k]];
            st->launch[i] = (int64_t)g * cfg->launch_delay + rng_randint(&ctx->rng, 0, cfg->launch_jitter);
            st->lift[i] = (double)rng_randint(&ctx->rng, cfg->arc_height.start, cfg->arc_height.end);
        }
    }
    charidgrouping_free(&groups);
    free(index_of);

    const Canvas *cv = &ctx->terminal.canvas;
    st->origin = coord_new(cv->center_column, 0);
    st->top_row = cv->top;
    st->last_frame = 0;
    for (size_t i = 0; i < n; i++) {
        int64_t end = st->launch[i] + cfg->flight_frames + cfg->cooldown_frames;
        if (end > st->last_frame) {
            st->last_frame = end;
        }
        CharId id = st->cast.ids[i];
        orig_layer(ctx, id, 1);
        for (int64_t k = 0; k < cfg->trail_length; k++) {
            CharId tid = terminal_add_character(&ctx->terminal, " ", st->origin);
            ctx->terminal.arena.items[tid].uses_input_preexisting_colors = false;
            st->trail[i * (size_t)cfg->trail_length + (size_t)k] = tid;
        }
    }
    return 0;
}

// Position along the arc at eased progress e: straight from the origin to
// home, lifted by a half sine so every flight bows upward.
static Coord arc_point(const Comet *st, const EffectCharacter *ch, size_t i, double e) {
    double x = (double)st->origin.column + (double)(ch->input_coord.column - st->origin.column) * e;
    double y = (double)st->origin.row + (double)(ch->input_coord.row - st->origin.row) * e;
    y += sin(e * M_PI) * st->lift[i];
    // Flights skim the top edge rather than leaving a short canvas.
    double top = (double)st->top_row;
    if (y > top) {
        y = top;
    }
    return coord_new((int64_t)floor(x + 0.5), (int64_t)floor(y + 0.5));
}

static void hide_trail(EngineCtx *ctx, Comet *st, size_t i) {
    for (int64_t k = 0; k < st->config.trail_length; k++) {
        orig_hide(ctx, st->trail[i * (size_t)st->config.trail_length + (size_t)k]);
    }
}

static void fly(EngineCtx *ctx, Comet *st, size_t i, int64_t f) {
    const CometConfig *cfg = &st->config;
    CharId id = st->cast.ids[i];
    const EffectCharacter *ch = &ctx->terminal.arena.items[id];
    const Color *fin = st->cast.has_final[i] ? &st->cast.final[i] : &cfg->comet_color;
    double p = (double)(f - st->launch[i]) / (double)cfg->flight_frames;
    Color head = orig_mix(fin, &cfg->comet_color, 0.6);
    orig_layer(ctx, id, 2);
    orig_look(ctx, id, ch->input_symbol, &head, NULL);
    orig_place(ctx, id, arc_point(st, ch, i, easing_ease(&cfg->flight_ease, p)));
    const Color *bg = &ctx->terminal.config.terminal_background_color;
    size_t n_sym = cfg->trail_symbols.len;
    for (int64_t k = 0; k < cfg->trail_length; k++) {
        CharId tid = st->trail[i * (size_t)cfg->trail_length + (size_t)k];
        double pk = p - (double)(k + 1) * TRAIL_SPACING / (double)cfg->flight_frames;
        if (pk < 0.0) {
            orig_hide(ctx, tid);
            continue;
        }
        double age = (double)(k + 1) / (double)(cfg->trail_length + 1);
        size_t s = (size_t)(age * (double)n_sym);
        Color c = orig_mix(fin, bg, age);
        orig_look(ctx, tid, cfg->trail_symbols.items[s < n_sym ? s : n_sym - 1], &c, NULL);
        orig_place(ctx, tid, arc_point(st, ch, i, easing_ease(&cfg->flight_ease, pk)));
    }
}

static void cool(EngineCtx *ctx, Comet *st, size_t i, int64_t f) {
    const CometConfig *cfg = &st->config;
    CharId id = st->cast.ids[i];
    const EffectCharacter *ch = &ctx->terminal.arena.items[id];
    const Color *fin = st->cast.has_final[i] ? &st->cast.final[i] : NULL;
    int64_t age = f - st->launch[i] - cfg->flight_frames;
    double p = (double)age / (double)(cfg->cooldown_frames ? cfg->cooldown_frames : 1);
    orig_layer(ctx, id, 1);
    orig_place(ctx, id, ch->input_coord);
    if (p >= 1.0) {
        orig_look(ctx, id, ch->input_symbol, fin, NULL);
        st->state[i] = 3;
        return;
    }
    Color c = fin ? orig_mix(&cfg->comet_color, fin, p) : cfg->comet_color;
    // The landing flash: the cell behind the character glows and decays.
    const Color *bg = &ctx->terminal.config.terminal_background_color;
    Color glow = orig_mix(bg, fin ? fin : &cfg->comet_color, 0.45 * (1.0 - p) * (1.0 - p));
    orig_look(ctx, id, ch->input_symbol, &c, p < 0.75 ? &glow : NULL);
}

static const char *comet_next_frame(Effect *self, EngineCtx *ctx) {
    Comet *st = self->state;
    int64_t f = st->frame;
    if (f > st->last_frame) {
        return NULL;
    }
    for (size_t i = 0; i < st->cast.len; i++) {
        int64_t since = f - st->launch[i];
        if (st->state[i] == 3 || since < 0) {
            continue;
        }
        if (since < st->config.flight_frames) {
            st->state[i] = 1;
            fly(ctx, st, i, f);
        } else {
            if (st->state[i] == 1 || st->state[i] == 0) {
                hide_trail(ctx, st, i);
            }
            st->state[i] = 2;
            cool(ctx, st, i, f);
        }
    }
    st->frame++;
    return engine_frame(ctx);
}

static void comet_destroy(Effect *self) {
    Comet *st = self->state;
    if (!st) {
        return;
    }
    orig_cast_free(&st->cast);
    free(st->launch);
    free(st->lift);
    free(st->trail);
    free(st->state);
    free(st);
    free(self);
}

static const EffectOps COMET_OPS = {comet_build, comet_next_frame, comet_destroy, NULL};

Effect *comet_make(const void *cfg) {
    Comet *st = calloc(1, sizeof(Comet));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const CometConfig *)cfg;
    effect->ops = &COMET_OPS;
    effect->state = st;
    return effect;
}

const EffectEntry comet_entry = {
    "comet",
    (const EffOptSpec[]){
        EF_SPEC("launch-order", 0, EF_CHAR_GROUP, offsetof(CometConfig, launch_order)),
        EF_SPEC("launch-delay", 0, EF_NONNEG_INT, offsetof(CometConfig, launch_delay)),
        EF_SPEC("launch-jitter", 0, EF_NONNEG_INT, offsetof(CometConfig, launch_jitter)),
        EF_SPEC("flight-frames", 0, EF_POS_INT, offsetof(CometConfig, flight_frames)),
        EF_SPEC("flight-ease", 0, EF_EASING, offsetof(CometConfig, flight_ease)),
        EF_SPEC("arc-height", 0, EF_INT_RANGE, offsetof(CometConfig, arc_height)),
        EF_SPEC("trail-length", 0, EF_NONNEG_INT, offsetof(CometConfig, trail_length)),
        EF_SPEC("trail-symbols", 0, EF_STRING_LIST, offsetof(CometConfig, trail_symbols)),
        EF_SPEC("comet-color", 0, EF_COLOR, offsetof(CometConfig, comet_color)),
        EF_SPEC("cooldown-frames", 0, EF_NONNEG_INT, offsetof(CometConfig, cooldown_frames)),
        EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(CometConfig, final_gradient_stops)),
        EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(CometConfig, final_gradient_steps)),
        EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(CometConfig, final_gradient_direction)),
    },
    13,
    sizeof(CometConfig),
    comet_config_defaults,
    comet_free_config,
    comet_make,
};
