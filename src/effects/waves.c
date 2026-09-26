#include "effects/waves.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/graphics.h"

typedef struct {
    WavesConfig config;
    CharIdGrouping pending;
    size_t pending_head;
} Waves;

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

static void push_symbol_default(StringList *list, const char *symbol) {
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 4;
        char **grown = realloc(list->items, cap * sizeof(char *));
        if (!grown) {
            return;
        }
        list->items = grown;
        list->cap = cap;
    }
    char *copy = malloc(strlen(symbol) + 1);
    strcpy(copy, symbol);
    list->items[list->len++] = copy;
}

static int parse_wave_direction(const char *value, void *dst) {
    CharacterGroup *g = dst;
    if (strcmp(value, "column_left_to_right") == 0) {
        *g = CG_COLUMN_LEFT_TO_RIGHT;
    } else if (strcmp(value, "column_right_to_left") == 0) {
        *g = CG_COLUMN_RIGHT_TO_LEFT;
    } else if (strcmp(value, "row_top_to_bottom") == 0) {
        *g = CG_ROW_TOP_TO_BOTTOM;
    } else if (strcmp(value, "row_bottom_to_top") == 0) {
        *g = CG_ROW_BOTTOM_TO_TOP;
    } else if (strcmp(value, "center_to_outside") == 0) {
        *g = CG_CENTER_TO_OUTSIDE;
    } else if (strcmp(value, "outside_to_center") == 0) {
        *g = CG_OUTSIDE_TO_CENTER;
    } else {
        return -1;
    }
    return 0;
}

void waves_config_defaults(void *cfg_ptr) {
    WavesConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    const char *symbols[15] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█", "▇", "▆", "▅", "▄", "▃", "▂", "▁"};
    for (size_t i = 0; i < 15; i++) {
        push_symbol_default(&cfg->wave_symbols, symbols[i]);
    }
    push_color_default(&cfg->wave_gradient_stops, "f0ff65");
    push_color_default(&cfg->wave_gradient_stops, "ffb102");
    push_color_default(&cfg->wave_gradient_stops, "31a0d4");
    push_color_default(&cfg->wave_gradient_stops, "ffb102");
    push_color_default(&cfg->wave_gradient_stops, "f0ff65");
    push_int_default(&cfg->wave_gradient_steps, 6);
    cfg->wave_count = 7;
    cfg->wave_length = 2;
    cfg->wave_direction = CG_COLUMN_LEFT_TO_RIGHT;
    easing_parse("in_out_sine", &cfg->wave_easing);
    push_color_default(&cfg->final_gradient_stops, "ffb102");
    push_color_default(&cfg->final_gradient_stops, "31a0d4");
    push_color_default(&cfg->final_gradient_stops, "f0ff65");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_DIAGONAL;
}

void waves_free_config(void *cfg_ptr) {
    WavesConfig *cfg = cfg_ptr;
    for (size_t i = 0; i < cfg->wave_symbols.len; i++) {
        free(cfg->wave_symbols.items[i]);
    }
    free(cfg->wave_symbols.items);
    free(cfg->wave_gradient_stops.items);
    free(cfg->wave_gradient_steps.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int waves_build(Effect *self, EngineCtx *ctx) {
    Waves *st = self->state;
    WavesConfig *cfg = &st->config;

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
    Gradient wave_gradient;
    if (gradient_new(cfg->wave_gradient_stops.items, cfg->wave_gradient_stops.len, cfg->wave_gradient_steps.items,
                     cfg->wave_gradient_steps.len, false, false, &wave_gradient) != 0) {
        coordcolormap_free(&mapping);
        gradient_free(&final_gradient);
        return -1;
    }

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    size_t n = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);

    Easing no_ease;
    memset(&no_ease, 0, sizeof(no_ease));

    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        bool has_input_fg = ch->animation.has_input_fg;
        Color input_fg = ch->animation.input_fg_color;
        bool has_input_bg = ch->animation.has_input_bg;
        Color input_bg = ch->animation.input_bg_color;
        Coord input_coord = ch->input_coord;
        bool uses_pre = ch->uses_input_preexisting_colors;
        char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
        strcpy(input_symbol, ch->input_symbol);

        ColorPair final_colors;
        memset(&final_colors, 0, sizeof(final_colors));
        if (dynamic) {
            final_colors.has_fg = has_input_fg;
            final_colors.fg = input_fg;
            final_colors.has_bg = has_input_bg;
            final_colors.bg = input_bg;
        } else {
            const Color *mapped = coordcolormap_get(&mapping, input_coord);
            final_colors.has_fg = true;
            if (mapped) {
                final_colors.fg = *mapped;
            }
        }

        const char *ws = animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, true, cfg->wave_easing,
                                             "", uses_pre);
        char wave_scn[32];
        wave_scn[0] = '\0';
        if (ws) {
            strncpy(wave_scn, ws, sizeof(wave_scn) - 1);
            wave_scn[sizeof(wave_scn) - 1] = '\0';
        }
        Scene *wave_scene = (Scene *)om_get(&ch->animation.scenes, wave_scn);
        const char *const *symbols = (const char *const *)cfg->wave_symbols.items;
        for (int64_t w = 0; w < cfg->wave_count; w++) {
            if (scene_apply_gradient_to_symbols(wave_scene, symbols, cfg->wave_symbols.len, cfg->wave_length,
                                                &wave_gradient, NULL) != 0) {
                rc = -1;
                break;
            }
        }

        char final_scn[32];
        final_scn[0] = '\0';
        if (rc == 0) {
            const char *fs =
                animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, no_ease, "", uses_pre);
            if (fs) {
                strncpy(final_scn, fs, sizeof(final_scn) - 1);
                final_scn[sizeof(final_scn) - 1] = '\0';
            }
            Scene *scene = (Scene *)om_get(&ch->animation.scenes, final_scn);
            Color last_wave = wave_gradient.spectrum[wave_gradient.len - 1];
            if (dynamic) {
                if (!final_colors.has_fg && !final_colors.has_bg) {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    if (scene_add_frame(scene, input_symbol, 10, &vp) != 0) {
                        rc = -1;
                    }
                } else {
                    Gradient fg_grad;
                    Gradient bg_grad;
                    bool has_fg_grad = false;
                    bool has_bg_grad = false;
                    if (final_colors.has_fg) {
                        Color stops[2] = {last_wave, final_colors.fg};
                        if (gradient_new(stops, 2, cfg->final_gradient_steps.items,
                                         cfg->final_gradient_steps.len, false, false, &fg_grad) != 0) {
                            rc = -1;
                        } else {
                            has_fg_grad = true;
                        }
                    }
                    if (rc == 0 && final_colors.has_bg) {
                        Color stops[2] = {last_wave, final_colors.bg};
                        if (gradient_new(stops, 2, cfg->final_gradient_steps.items,
                                         cfg->final_gradient_steps.len, false, false, &bg_grad) != 0) {
                            rc = -1;
                        } else {
                            has_bg_grad = true;
                        }
                    }
                    if (rc == 0) {
                        scene = (Scene *)om_get(&ch->animation.scenes, final_scn);
                        const char *syms[1] = {input_symbol};
                        if (scene_apply_gradient_to_symbols(scene, syms, 1, 10, has_fg_grad ? &fg_grad : NULL,
                                                            has_bg_grad ? &bg_grad : NULL) != 0) {
                            rc = -1;
                        }
                    }
                    if (rc == 0 && !final_colors.has_fg) {
                        scene = (Scene *)om_get(&ch->animation.scenes, final_scn);
                        VisualParams vp;
                        memset(&vp, 0, sizeof(vp));
                        vp.has_colors = true;
                        vp.colors.has_bg = final_colors.has_bg;
                        vp.colors.bg = final_colors.bg;
                        if (scene_add_frame(scene, input_symbol, 10, &vp) != 0) {
                            rc = -1;
                        }
                    }
                    if (has_fg_grad) {
                        gradient_free(&fg_grad);
                    }
                    if (has_bg_grad) {
                        gradient_free(&bg_grad);
                    }
                }
            } else {
                Color stops[2] = {last_wave, final_colors.fg};
                Gradient final_scene_gradient;
                if (gradient_new(stops, 2, cfg->final_gradient_steps.items, cfg->final_gradient_steps.len, false,
                                 false, &final_scene_gradient) != 0) {
                    rc = -1;
                } else {
                    for (size_t s = 0; s < final_scene_gradient.len; s++) {
                        VisualParams vp;
                        memset(&vp, 0, sizeof(vp));
                        vp.has_colors = true;
                        vp.colors.has_fg = true;
                        vp.colors.fg = final_scene_gradient.spectrum[s];
                        if (scene_add_frame(scene, input_symbol, 10, &vp) != 0) {
                            rc = -1;
                            break;
                        }
                    }
                    gradient_free(&final_scene_gradient);
                }
            }
        }

        if (rc == 0) {
            CallerKey caller;
            memset(&caller, 0, sizeof(caller));
            caller.kind = CALLER_SCENE;
            caller.id = wave_scn;
            EventAction action;
            memset(&action, 0, sizeof(action));
            action.kind = ACTION_ACTIVATE_SCENE;
            action.has_id = true;
            action.id = final_scn;
            if (engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
                rc = -1;
            } else {
                engine_activate_scene(ctx, self, id, wave_scn);
                if (dynamic) {
                    ch = &ctx->terminal.arena.items[id];
                    animation_set_appearance(&ch->animation, uses_pre, input_symbol, &final_colors);
                }
            }
        }
        free(input_symbol);
    }
    free(characters);

    if (rc == 0) {
        st->pending = terminal_get_characters_grouped(&ctx->terminal, character_filter_default(),
                                                      cfg->wave_direction);
        st->pending_head = 0;
    }

    coordcolormap_free(&mapping);
    gradient_free(&wave_gradient);
    gradient_free(&final_gradient);
    return rc;
}

static const char *waves_next_frame(Effect *self, EngineCtx *ctx) {
    Waves *st = self->state;
    if (st->pending_head < st->pending.len || !ac_is_empty(&ctx->active_characters)) {
        if (st->pending_head < st->pending.len) {
            CharIdBucket column = st->pending.buckets[st->pending_head++];
            for (size_t i = 0; i < column.len; i++) {
                terminal_set_character_visibility(&ctx->terminal, column.items[i], true);
                ac_insert(&ctx->active_characters, column.items[i]);
            }
        }
        engine_update(ctx, self);
        return engine_frame(ctx);
    }
    return NULL;
}

static void waves_destroy(Effect *self) {
    Waves *st = self->state;
    if (!st) {
        return;
    }
    charidgrouping_free(&st->pending);
    free(st);
    free(self);
}

static const EffectOps WAVES_OPS = {waves_build, waves_next_frame, waves_destroy, NULL};

Effect *waves_make(const void *cfg) {
    Waves *st = calloc(1, sizeof(Waves));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const WavesConfig *)cfg;
    effect->ops = &WAVES_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec waves_specs[] = {
    {"wave-symbols", 0, EF_STRING_LIST, offsetof(WavesConfig, wave_symbols), NULL},
    EF_SPEC("wave-gradient-stops", 0, EF_COLOR_LIST, offsetof(WavesConfig, wave_gradient_stops)),
    EF_SPEC("wave-gradient-steps", 0, EF_INT_LIST, offsetof(WavesConfig, wave_gradient_steps)),
    EF_SPEC("wave-count", 0, EF_POS_INT, offsetof(WavesConfig, wave_count)),
    EF_SPEC("wave-length", 0, EF_POS_INT, offsetof(WavesConfig, wave_length)),
    {"wave-direction", 0, EF_CUSTOM, offsetof(WavesConfig, wave_direction), parse_wave_direction},
    EF_SPEC("wave-easing", 0, EF_EASING, offsetof(WavesConfig, wave_easing)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(WavesConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(WavesConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(WavesConfig, final_gradient_direction)),
};

const EffectEntry waves_entry = {
    "waves",
    waves_specs,
    sizeof(waves_specs) / sizeof(waves_specs[0]),
    sizeof(WavesConfig),
    waves_config_defaults,
    waves_free_config,
    waves_make,
};
