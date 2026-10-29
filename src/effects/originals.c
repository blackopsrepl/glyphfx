#include "effects/originals.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "engine/animation.h"
#include "engine/render_log.h"
#include "engine/terminal.h"

int orig_cast_build(EngineCtx *ctx, const ColorList *stops, const IntList *steps, GradientDirection direction,
                    OrigCast *out) {
    memset(out, 0, sizeof(*out));
    Terminal *t = &ctx->terminal;
    Gradient gradient;
    if (gradient_new(stops->items, stops->len, steps->items, steps->len, false, false, &gradient) != 0) {
        return -1;
    }
    CoordColorMap mapping;
    if (gradient_build_coordinate_color_mapping(&gradient, t->canvas.text_bottom, t->canvas.text_top,
                                                t->canvas.text_left, t->canvas.text_right, direction,
                                                &mapping) != 0) {
        gradient_free(&gradient);
        return -1;
    }
    size_t n = 0;
    out->ids = terminal_get_characters(t, &ctx->rng, character_filter_default(), CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);
    out->final = calloc(n ? n : 1, sizeof(Color));
    out->has_final = calloc(n ? n : 1, sizeof(bool));
    out->len = n;
    bool dynamic = t->config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    for (size_t i = 0; i < n; i++) {
        EffectCharacter *ch = &t->arena.items[out->ids[i]];
        if (dynamic) {
            out->has_final[i] = ch->animation.has_input_fg;
            out->final[i] = ch->animation.input_fg_color;
        } else {
            const Color *c = coordcolormap_get(&mapping, ch->input_coord);
            out->has_final[i] = true;
            out->final[i] = c ? *c : gradient.spectrum[gradient.len - 1];
        }
    }
    coordcolormap_free(&mapping);
    gradient_free(&gradient);
    return 0;
}

void orig_cast_free(OrigCast *cast) {
    free(cast->ids);
    free(cast->final);
    free(cast->has_final);
    memset(cast, 0, sizeof(*cast));
}

double orig_clamp01(double x) {
    return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
}

double orig_smooth(double x) {
    x = orig_clamp01(x);
    return x * x * (3.0 - 2.0 * x);
}

Color orig_mix(const Color *a, const Color *b, double t) {
    double q = floor(orig_clamp01(t) * ORIG_MIX_STEPS + 0.5) / ORIG_MIX_STEPS;
    if (q <= 0.0) {
        return *a;
    }
    if (q >= 1.0) {
        return *b;
    }
    uint8_t x[3], y[3], m[3];
    color_rgb_ints(a, x);
    color_rgb_ints(b, y);
    for (int i = 0; i < 3; i++) {
        m[i] = (uint8_t)floor(x[i] + (y[i] - x[i]) * q + 0.5);
    }
    return color_from_rgb(m[0], m[1], m[2]);
}

Color orig_ramp(const Color *stops, size_t n, double t) {
    if (n == 1) {
        return stops[0];
    }
    double s = orig_clamp01(t) * (double)(n - 1);
    size_t i = (size_t)s;
    if (i >= n - 1) {
        i = n - 2;
    }
    return orig_mix(&stops[i], &stops[i + 1], s - (double)i);
}

void orig_look(EngineCtx *ctx, CharId id, const char *symbol, const Color *fg, const Color *bg) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    ColorPair colors;
    memset(&colors, 0, sizeof(colors));
    if (fg) {
        colors.has_fg = true;
        colors.fg = *fg;
    }
    if (bg) {
        colors.has_bg = true;
        colors.bg = *bg;
    }
    animation_set_appearance(&ch->animation, ch->uses_input_preexisting_colors, symbol, &colors);
}

void orig_place(EngineCtx *ctx, CharId id, Coord coord) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    Coord cur = ch->motion.current_coord;
    if (cur.column != coord.column || cur.row != coord.row) {
        motion_set_coordinate(&ch->motion, coord);
    }
    terminal_set_character_visibility(&ctx->terminal, id, canvas_coord_is_in_canvas(&ctx->terminal.canvas, coord));
}

void orig_hide(EngineCtx *ctx, CharId id) {
    terminal_set_character_visibility(&ctx->terminal, id, false);
}

void orig_layer(EngineCtx *ctx, CharId id, int64_t layer) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    if (ch->layer != layer) {
        ch->layer = layer;
        renderer_layer(id, layer);
    }
}

void orig_push_color(ColorList *list, const char *hex) {
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 4;
        list->items = realloc(list->items, cap * sizeof(Color));
        list->cap = cap;
    }
    color_from_hex(hex, &list->items[list->len++]);
}

void orig_push_int(IntList *list, int64_t v) {
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 4;
        list->items = realloc(list->items, cap * sizeof(int64_t));
        list->cap = cap;
    }
    list->items[list->len++] = v;
}

void orig_push_symbol(StringList *list, const char *symbol) {
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 4;
        list->items = realloc(list->items, cap * sizeof(char *));
        list->cap = cap;
    }
    char *copy = malloc(strlen(symbol) + 1);
    strcpy(copy, symbol);
    list->items[list->len++] = copy;
}

void orig_free_symbols(StringList *list) {
    for (size_t i = 0; i < list->len; i++) {
        free(list->items[i]);
    }
    free(list->items);
}
