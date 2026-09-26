#include "effects/decrypt.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/graphics.h"

typedef enum {
    DECRYPT_TYPING,
    DECRYPT_DECRYPTING,
} DecryptPhase;

typedef struct {
    DecryptConfig config;
    StringList encrypted_symbols;
    CharId *typing_pending;
    size_t typing_len;
    size_t typing_cap;
    size_t typing_head;
    CharId *decrypting_pending;
    size_t decrypting_len;
    size_t decrypting_cap;
    DecryptPhase phase;
    ColorPair *final_colors;
    bool *final_colors_present;
    size_t final_colors_len;
} Decrypt;

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
        size_t cap = list->cap ? list->cap * 2 : 64;
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

static size_t utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static void decrypt_make_encrypted_symbols(Decrypt *st) {
    const uint32_t ranges[4][2] = {{33, 127}, {9608, 9632}, {9472, 9599}, {174, 452}};
    for (size_t r = 0; r < 4; r++) {
        for (uint32_t n = ranges[r][0]; n < ranges[r][1]; n++) {
            char buf[5];
            size_t len = utf8_encode(n, buf);
            buf[len] = '\0';
            push_symbol_default(&st->encrypted_symbols, buf);
        }
    }
}

void decrypt_config_defaults(void *cfg_ptr) {
    DecryptConfig *cfg = cfg_ptr;
    memset(cfg, 0, sizeof(*cfg));
    cfg->typing_speed = 2;
    push_color_default(&cfg->ciphertext_colors, "008000");
    push_color_default(&cfg->ciphertext_colors, "00cb00");
    push_color_default(&cfg->ciphertext_colors, "00ff00");
    push_color_default(&cfg->final_gradient_stops, "eda000");
    push_int_default(&cfg->final_gradient_steps, 12);
    cfg->final_gradient_direction = GRADIENT_VERTICAL;
}

void decrypt_free_config(void *cfg_ptr) {
    DecryptConfig *cfg = cfg_ptr;
    free(cfg->ciphertext_colors.items);
    free(cfg->final_gradient_stops.items);
    free(cfg->final_gradient_steps.items);
}

static int decrypt_make_decrypting_scenes(Effect *self, EngineCtx *ctx, CharId id) {
    Decrypt *st = self->state;
    DecryptConfig *cfg = &st->config;
    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;

    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    bool has_input_fg = ch->animation.has_input_fg;
    Color input_fg = ch->animation.input_fg_color;
    bool has_input_bg = ch->animation.has_input_bg;
    Color input_bg = ch->animation.input_bg_color;
    bool uses_pre = ch->uses_input_preexisting_colors;
    char *input_symbol = malloc(strlen(ch->input_symbol) + 1);
    strcpy(input_symbol, ch->input_symbol);

    animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, (Easing){0}, "fast_decrypt", uses_pre);
    Scene *fast = (Scene *)om_get(&ch->animation.scenes, "fast_decrypt");
    Color color = cfg->ciphertext_colors.items[rng_choice_index(&ctx->rng, cfg->ciphertext_colors.len)];
    int rc = 0;
    for (int i = 0; i < 80; i++) {
        const char *symbol =
            st->encrypted_symbols.items[rng_choice_index(&ctx->rng, st->encrypted_symbols.len)];
        VisualParams vp;
        memset(&vp, 0, sizeof(vp));
        vp.has_colors = true;
        vp.colors.has_fg = true;
        vp.colors.fg = color;
        if (scene_add_frame(fast, symbol, 2, &vp) != 0) {
            rc = -1;
            break;
        }
    }

    if (rc == 0) {
        ch = &ctx->terminal.arena.items[id];
        animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, (Easing){0}, "slow_decrypt",
                            uses_pre);
        Scene *slow = (Scene *)om_get(&ch->animation.scenes, "slow_decrypt");
        int64_t count = rng_randint(&ctx->rng, 1, 15);
        for (int64_t i = 0; i < count; i++) {
            const char *symbol =
                st->encrypted_symbols.items[rng_choice_index(&ctx->rng, st->encrypted_symbols.len)];
            int64_t duration;
            if (rng_randint(&ctx->rng, 0, 100) <= 30) {
                duration = rng_randrange(&ctx->rng, 35, 60);
            } else {
                duration = rng_randrange(&ctx->rng, 3, 6);
            }
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = color;
            if (scene_add_frame(slow, symbol, duration, &vp) != 0) {
                rc = -1;
                break;
            }
        }
    }

    if (rc == 0) {
        ch = &ctx->terminal.arena.items[id];
        animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, (Easing){0}, "discovered",
                            uses_pre);
        Scene *scene = (Scene *)om_get(&ch->animation.scenes, "discovered");
        Color white;
        color_from_hex("ffffff", &white);
        if (dynamic) {
            Gradient fg_grad;
            Gradient bg_grad;
            bool has_fg_grad = false;
            bool has_bg_grad = false;
            if (has_input_fg) {
                Color stops[2] = {white, input_fg};
                if (gradient_with_steps(stops, 2, 10, false, &fg_grad) == 0) {
                    has_fg_grad = true;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0 && has_input_bg) {
                Color stops[2] = {white, input_bg};
                if (gradient_with_steps(stops, 2, 10, false, &bg_grad) == 0) {
                    has_bg_grad = true;
                } else {
                    rc = -1;
                }
            }
            if (rc == 0) {
                if (has_fg_grad || has_bg_grad) {
                    const char *syms[1] = {input_symbol};
                    if (scene_apply_gradient_to_symbols(scene, syms, 1, 5, has_fg_grad ? &fg_grad : NULL,
                                                        has_bg_grad ? &bg_grad : NULL) != 0) {
                        rc = -1;
                    }
                } else {
                    VisualParams vp;
                    memset(&vp, 0, sizeof(vp));
                    vp.has_colors = true;
                    if (scene_add_frame(scene, input_symbol, 5, &vp) != 0) {
                        rc = -1;
                    }
                }
            }
            if (has_fg_grad) {
                gradient_free(&fg_grad);
            }
            if (has_bg_grad) {
                gradient_free(&bg_grad);
            }
        } else {
            Color final_fg;
            memset(&final_fg, 0, sizeof(final_fg));
            if ((size_t)id < st->final_colors_len && st->final_colors_present[id]) {
                final_fg = st->final_colors[id].fg;
            }
            Color stops[2] = {white, final_fg};
            Gradient discovered_gradient;
            if (gradient_with_steps(stops, 2, 10, false, &discovered_gradient) != 0) {
                rc = -1;
            } else {
                const char *syms[1] = {input_symbol};
                if (scene_apply_gradient_to_symbols(scene, syms, 1, 5, &discovered_gradient, NULL) != 0) {
                    rc = -1;
                }
                gradient_free(&discovered_gradient);
            }
        }
    }

    free(input_symbol);
    return rc;
}

static int decrypt_prepare_type(Effect *self, EngineCtx *ctx) {
    Decrypt *st = self->state;
    size_t n = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);
    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        bool uses_pre = ch->uses_input_preexisting_colors;
        animation_new_scene(&ch->animation, false, false, SYNC_DISTANCE, false, (Easing){0}, "typing", uses_pre);
        Scene *scene = (Scene *)om_get(&ch->animation.scenes, "typing");
        const char *blocks[4] = {"▉", "▓", "▒", "░"};
        for (size_t b = 0; b < 4; b++) {
            Color color = st->config.ciphertext_colors.items[rng_choice_index(
                &ctx->rng, st->config.ciphertext_colors.len)];
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = color;
            if (scene_add_frame(scene, blocks[b], 2, &vp) != 0) {
                rc = -1;
                break;
            }
        }
        if (rc == 0) {
            const char *symbol =
                st->encrypted_symbols.items[rng_choice_index(&ctx->rng, st->encrypted_symbols.len)];
            Color color =
                st->config.ciphertext_colors.items[rng_choice_index(&ctx->rng, st->config.ciphertext_colors.len)];
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors.has_fg = true;
            vp.colors.fg = color;
            if (scene_add_frame(scene, symbol, 1, &vp) != 0) {
                rc = -1;
            }
        }
        if (rc == 0) {
            if (st->typing_len == st->typing_cap) {
                size_t cap = st->typing_cap ? st->typing_cap * 2 : 64;
                st->typing_pending = realloc(st->typing_pending, cap * sizeof(CharId));
                st->typing_cap = cap;
            }
            st->typing_pending[st->typing_len++] = id;
        }
    }
    free(characters);
    return rc;
}

static int decrypt_prepare_decrypt(Effect *self, EngineCtx *ctx) {
    Decrypt *st = self->state;
    size_t n = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);
    int rc = 0;
    for (size_t i = 0; i < n && rc == 0; i++) {
        CharId id = characters[i];
        if (decrypt_make_decrypting_scenes(self, ctx, id) != 0) {
            rc = -1;
            break;
        }
        CallerKey caller;
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_SCENE;
        caller.id = "fast_decrypt";
        EventAction action;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_SCENE;
        action.has_id = true;
        action.id = "slow_decrypt";
        if (engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
            rc = -1;
            break;
        }
        memset(&caller, 0, sizeof(caller));
        caller.kind = CALLER_SCENE;
        caller.id = "slow_decrypt";
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_SCENE;
        action.has_id = true;
        action.id = "discovered";
        if (engine_register_event(ctx, id, EVENT_SCENE_COMPLETE, &caller, &action) != 0) {
            rc = -1;
            break;
        }
        engine_activate_scene(ctx, self, id, "fast_decrypt");
        if (st->decrypting_len == st->decrypting_cap) {
            size_t cap = st->decrypting_cap ? st->decrypting_cap * 2 : 64;
            st->decrypting_pending = realloc(st->decrypting_pending, cap * sizeof(CharId));
            st->decrypting_cap = cap;
        }
        st->decrypting_pending[st->decrypting_len++] = id;
    }
    free(characters);
    return rc;
}

static int decrypt_build(Effect *self, EngineCtx *ctx) {
    Decrypt *st = self->state;
    DecryptConfig *cfg = &st->config;

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

    bool dynamic = ctx->terminal.config.existing_color_handling == EXISTING_COLOR_DYNAMIC;
    size_t n = 0;
    CharId *characters = terminal_get_characters(&ctx->terminal, &ctx->rng, character_filter_default(),
                                                 CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);

    size_t arena_len = ctx->terminal.arena.len;
    st->final_colors_len = arena_len;
    st->final_colors = calloc(arena_len ? arena_len : 1, sizeof(ColorPair));
    st->final_colors_present = calloc(arena_len ? arena_len : 1, sizeof(bool));
    for (size_t i = 0; i < n; i++) {
        CharId id = characters[i];
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        ColorPair colors;
        memset(&colors, 0, sizeof(colors));
        if (dynamic) {
            colors.has_fg = ch->animation.has_input_fg;
            colors.fg = ch->animation.input_fg_color;
            colors.has_bg = ch->animation.has_input_bg;
            colors.bg = ch->animation.input_bg_color;
        } else {
            const Color *mapped = coordcolormap_get(&mapping, ch->input_coord);
            colors.has_fg = true;
            if (mapped) {
                colors.fg = *mapped;
            }
        }
        if ((size_t)id < st->final_colors_len) {
            st->final_colors[id] = colors;
            st->final_colors_present[id] = true;
        }
    }
    free(characters);

    int rc = decrypt_prepare_type(self, ctx);
    if (rc == 0) {
        rc = decrypt_prepare_decrypt(self, ctx);
    }

    coordcolormap_free(&mapping);
    gradient_free(&final_gradient);
    return rc;
}

static char *decrypt_next_frame(Effect *self, EngineCtx *ctx) {
    Decrypt *st = self->state;
    if (st->phase == DECRYPT_TYPING) {
        if (st->typing_head < st->typing_len || !ac_is_empty(&ctx->active_characters)) {
            if (st->typing_head < st->typing_len && rng_randint(&ctx->rng, 0, 100) <= 75) {
                for (int64_t i = 0; i < st->config.typing_speed; i++) {
                    if (st->typing_head < st->typing_len) {
                        CharId next_character = st->typing_pending[st->typing_head++];
                        terminal_set_character_visibility(&ctx->terminal, next_character, true);
                        engine_activate_scene(ctx, self, next_character, "typing");
                        ac_insert(&ctx->active_characters, next_character);
                    }
                }
            }
            engine_update(ctx, self);
            return engine_frame(ctx);
        }
        ac_clear(&ctx->active_characters);
        ac_extend(&ctx->active_characters, st->decrypting_pending, st->decrypting_len);
        CharId *active = NULL;
        size_t active_len = 0;
        ac_snapshot(&ctx->active_characters, &active, &active_len);
        for (size_t i = 0; i < active_len; i++) {
            engine_activate_scene(ctx, self, active[i], "fast_decrypt");
        }
        free(active);
        st->phase = DECRYPT_DECRYPTING;
    }

    if (st->phase == DECRYPT_DECRYPTING) {
        if (!ac_is_empty(&ctx->active_characters)) {
            engine_update(ctx, self);
            return engine_frame(ctx);
        }
        return NULL;
    }
    return NULL;
}

static void decrypt_destroy(Effect *self) {
    Decrypt *st = self->state;
    if (!st) {
        return;
    }
    for (size_t i = 0; i < st->encrypted_symbols.len; i++) {
        free(st->encrypted_symbols.items[i]);
    }
    free(st->encrypted_symbols.items);
    free(st->typing_pending);
    free(st->decrypting_pending);
    free(st->final_colors);
    free(st->final_colors_present);
    free(st);
    free(self);
}

static const EffectOps DECRYPT_OPS = {decrypt_build, decrypt_next_frame, decrypt_destroy, NULL};

Effect *decrypt_make(const void *cfg) {
    Decrypt *st = calloc(1, sizeof(Decrypt));
    Effect *effect = calloc(1, sizeof(Effect));
    if (!st || !effect) {
        free(st);
        free(effect);
        return NULL;
    }
    st->config = *(const DecryptConfig *)cfg;
    st->phase = DECRYPT_TYPING;
    decrypt_make_encrypted_symbols(st);
    effect->ops = &DECRYPT_OPS;
    effect->state = st;
    return effect;
}

static const EffOptSpec decrypt_specs[] = {
    EF_SPEC("typing-speed", 0, EF_POS_INT, offsetof(DecryptConfig, typing_speed)),
    EF_SPEC("ciphertext-colors", 0, EF_COLOR_LIST, offsetof(DecryptConfig, ciphertext_colors)),
    EF_SPEC("final-gradient-stops", 0, EF_COLOR_LIST, offsetof(DecryptConfig, final_gradient_stops)),
    EF_SPEC("final-gradient-steps", 0, EF_INT_LIST, offsetof(DecryptConfig, final_gradient_steps)),
    EF_SPEC("final-gradient-direction", 0, EF_DIRECTION, offsetof(DecryptConfig, final_gradient_direction)),
};

const EffectEntry decrypt_entry = {
    "decrypt",
    decrypt_specs,
    sizeof(decrypt_specs) / sizeof(decrypt_specs[0]),
    sizeof(DecryptConfig),
    decrypt_config_defaults,
    decrypt_free_config,
    decrypt_make,
};
