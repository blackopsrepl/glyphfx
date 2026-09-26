#include "engine/animation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/hexterm.h"
#include "utils/strbuf.h"

static char *dup_cstr(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, len + 1);
    return out;
}

static bool resolve_color_code(bool has_color, const Color *color, bool no_color, bool use_xterm_colors,
                               ColorCode *out) {
    if (!has_color || no_color) {
        return false;
    }
    if (use_xterm_colors) {
        uint8_t code = color->is_xterm ? color->xterm : hex_to_xterm(color->hex);
        *out = colorcode_xterm(code);
    } else {
        *out = colorcode_rgb(color->hex);
    }
    return true;
}

// --- CharacterVisual ------------------------------------------------------

void vis_format(CharacterVisual *vis) {
    StrBuf sb;
    sb_init(&sb);
    if (vis->bold) {
        sb_puts(&sb, ANSI_BOLD);
    }
    if (vis->italic) {
        sb_puts(&sb, ANSI_ITALIC);
    }
    if (vis->underline) {
        sb_puts(&sb, ANSI_UNDERLINE);
    }
    if (vis->blink) {
        sb_puts(&sb, ANSI_BLINK);
    }
    if (vis->reverse) {
        sb_puts(&sb, ANSI_REVERSE);
    }
    if (vis->hidden) {
        sb_puts(&sb, ANSI_HIDDEN);
    }
    if (vis->strike) {
        sb_puts(&sb, ANSI_STRIKETHROUGH);
    }
    if (vis->has_fg_code) {
        ansi_fg(&vis->fg_code, &sb);
    }
    if (vis->has_bg_code) {
        ansi_bg(&vis->bg_code, &sb);
    }
    size_t symbol_len = strlen(vis->symbol);
    sb_puts(&sb, vis->symbol);
    if (sb.len != symbol_len) {
        sb_puts(&sb, ANSI_RESET_ALL);
    }
    free(vis->formatted);
    vis->formatted = sb_take(&sb);
}

void vis_init(CharacterVisual *vis, const char *symbol, const VisualParams *params) {
    memset(vis, 0, sizeof(*vis));
    vis->symbol = dup_cstr(symbol);
    vis->bold = params->bold;
    vis->dim = params->dim;
    vis->italic = params->italic;
    vis->underline = params->underline;
    vis->blink = params->blink;
    vis->reverse = params->reverse;
    vis->hidden = params->hidden;
    vis->strike = params->strike;
    vis->has_colors = params->has_colors;
    vis->colors = params->colors;
    vis->has_fg_code = params->has_fg_code;
    vis->fg_code = params->fg_code;
    vis->has_bg_code = params->has_bg_code;
    vis->bg_code = params->bg_code;
    vis_format(vis);
}

void vis_init_plain(CharacterVisual *vis, const char *symbol) {
    VisualParams params;
    memset(&params, 0, sizeof(params));
    vis_init(vis, symbol, &params);
}

void vis_free(CharacterVisual *vis) {
    free(vis->symbol);
    free(vis->formatted);
    vis->symbol = NULL;
    vis->formatted = NULL;
}

void vis_copy(CharacterVisual *dst, const CharacterVisual *src) {
    *dst = *src;
    dst->symbol = src->symbol ? dup_cstr(src->symbol) : NULL;
    dst->formatted = src->formatted ? dup_cstr(src->formatted) : NULL;
}

void vis_move(CharacterVisual *dst, CharacterVisual *src) {
    if (dst == src) {
        return;
    }
    vis_free(dst);
    *dst = *src;
    memset(src, 0, sizeof(*src));
}

// --- IndexDeque -----------------------------------------------------------

void iq_init(IndexDeque *q) {
    q->items = NULL;
    q->len = 0;
    q->cap = 0;
    q->head = 0;
}

void iq_free(IndexDeque *q) {
    free(q->items);
    iq_init(q);
}

static void iq_compact(IndexDeque *q) {
    if (q->head > 0) {
        memmove(q->items, q->items + q->head, q->len * sizeof(size_t));
        q->head = 0;
    }
}

void iq_push_back(IndexDeque *q, size_t v) {
    if (q->head + q->len == q->cap) {
        if (q->head > 0) {
            iq_compact(q);
        } else {
            size_t cap = q->cap ? q->cap * 2 : 16;
            size_t *grown = realloc(q->items, cap * sizeof(size_t));
            if (!grown) {
                return;
            }
            q->items = grown;
            q->cap = cap;
        }
    }
    q->items[q->head + q->len] = v;
    q->len++;
}

bool iq_pop_front(IndexDeque *q, size_t *out) {
    if (q->len == 0) {
        return false;
    }
    *out = q->items[q->head];
    q->head++;
    q->len--;
    if (q->len == 0) {
        q->head = 0;
    }
    return true;
}

static bool iq_peek_front(const IndexDeque *q, size_t *out) {
    if (q->len == 0) {
        return false;
    }
    *out = q->items[q->head];
    return true;
}

bool iq_at(const IndexDeque *q, size_t index, size_t *out) {
    if (index >= q->len) {
        return false;
    }
    *out = q->items[q->head + index];
    return true;
}

bool iq_back(const IndexDeque *q, size_t *out) {
    if (q->len == 0) {
        return false;
    }
    *out = q->items[q->head + q->len - 1];
    return true;
}

void iq_append(IndexDeque *dst, IndexDeque *src) {
    size_t v;
    while (iq_pop_front(src, &v)) {
        iq_push_back(dst, v);
    }
}

// --- Scene ----------------------------------------------------------------

static Scene *scene_new(const char *scene_id, bool is_looping, bool has_sync, SyncMetric sync, bool has_ease,
                        Easing ease, bool no_color, bool use_xterm_colors) {
    Scene *scene = calloc(1, sizeof(Scene));
    if (!scene) {
        return NULL;
    }
    scene->scene_id = dup_cstr(scene_id);
    scene->is_looping = is_looping;
    scene->no_color = no_color;
    scene->use_xterm_colors = use_xterm_colors;
    scene->has_sync = has_sync;
    scene->sync = sync;
    scene->has_ease = has_ease;
    scene->ease = ease;
    iq_init(&scene->frames);
    iq_init(&scene->played_frames);
    return scene;
}

void scene_free(Scene *scene) {
    if (!scene) {
        return;
    }
    for (size_t i = 0; i < scene->all_frames_len; i++) {
        vis_free(&scene->all_frames[i].visual);
    }
    free(scene->all_frames);
    free(scene->scene_id);
    free(scene->frame_index_map);
    iq_free(&scene->frames);
    iq_free(&scene->played_frames);
    free(scene);
}

int scene_add_frame(Scene *scene, const char *symbol, int64_t duration, const VisualParams *params_in) {
    VisualParams params = *params_in;
    if (scene->has_preexisting_colors) {
        params.colors = scene->preexisting_colors;
        params.has_colors = true;
    }
    if (scene->preexisting_bold) {
        params.bold = true;
    }
    if (params.has_colors) {
        params.has_fg_code =
            resolve_color_code(params.colors.has_fg, &params.colors.fg, scene->no_color, scene->use_xterm_colors,
                               &params.fg_code);
        params.has_bg_code =
            resolve_color_code(params.colors.has_bg, &params.colors.bg, scene->no_color, scene->use_xterm_colors,
                               &params.bg_code);
    } else {
        params.has_fg_code = false;
        params.has_bg_code = false;
    }
    if (duration < 1) {
        return -1;
    }
    if (scene->all_frames_len == scene->all_frames_cap) {
        size_t cap = scene->all_frames_cap ? scene->all_frames_cap * 2 : 8;
        Frame *grown = realloc(scene->all_frames, cap * sizeof(Frame));
        if (!grown) {
            return -1;
        }
        scene->all_frames = grown;
        scene->all_frames_cap = cap;
    }
    size_t frame_index = scene->all_frames_len++;
    Frame *frame = &scene->all_frames[frame_index];
    memset(frame, 0, sizeof(*frame));
    vis_init(&frame->visual, symbol, &params);
    frame->duration = duration;
    frame->ticks_elapsed = 0;
    iq_push_back(&scene->frames, frame_index);
    for (int64_t i = 0; i < duration; i++) {
        if (scene->frame_index_map_len == scene->frame_index_map_cap) {
            size_t cap = scene->frame_index_map_cap ? scene->frame_index_map_cap * 2 : 16;
            size_t *grown = realloc(scene->frame_index_map, cap * sizeof(size_t));
            if (!grown) {
                return -1;
            }
            scene->frame_index_map = grown;
            scene->frame_index_map_cap = cap;
        }
        scene->frame_index_map[scene->frame_index_map_len++] = frame_index;
        scene->easing_total_steps += 1;
    }
    return 0;
}

int scene_activate(const Scene *scene, CharacterVisual *out) {
    size_t head;
    if (!iq_peek_front(&scene->frames, &head)) {
        return -1;
    }
    vis_copy(out, &scene->all_frames[head].visual);
    return 0;
}

void scene_get_next_visual(Scene *scene, CharacterVisual *out) {
    size_t head;
    if (!iq_peek_front(&scene->frames, &head)) {
        return;
    }
    vis_copy(out, &scene->all_frames[head].visual);
    scene->all_frames[head].ticks_elapsed += 1;
    if (scene->all_frames[head].ticks_elapsed == scene->all_frames[head].duration) {
        scene->all_frames[head].ticks_elapsed = 0;
        size_t popped = 0;
        (void)iq_pop_front(&scene->frames, &popped);
        iq_push_back(&scene->played_frames, popped);
        if (scene->is_looping && iq_len(&scene->frames) == 0) {
            iq_append(&scene->frames, &scene->played_frames);
        }
    }
}

void scene_reset(Scene *scene) {
    size_t idx;
    while (iq_pop_front(&scene->frames, &idx)) {
        scene->all_frames[idx].ticks_elapsed = 0;
        iq_push_back(&scene->played_frames, idx);
    }
    iq_append(&scene->frames, &scene->played_frames);
    scene->easing_current_step = 0;
}

static size_t count_codepoints(const char *s) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p & 0xC0) != 0x80) {
            n++;
        }
    }
    return n;
}

int scene_apply_gradient_to_symbols(Scene *scene, const char *const *symbols, size_t n_symbols, int64_t duration,
                                    const Gradient *fg, const Gradient *bg) {
    bool fg_has = fg && fg->len > 0;
    bool bg_has = bg && bg->len > 0;
    if (!fg && !bg) {
        return -1;
    }
    if (!fg_has && !bg_has) {
        return -1;
    }
    for (size_t i = 0; i < n_symbols; i++) {
        if (count_codepoints(symbols[i]) > 1) {
            return -1;
        }
    }

    ColorPair *pairs = NULL;
    size_t pairs_len = 0;
    if (fg_has && bg_has) {
        const Color *larger = fg->len >= bg->len ? fg->spectrum : bg->spectrum;
        const Color *smaller = fg->len >= bg->len ? bg->spectrum : fg->spectrum;
        size_t larger_len = fg->len >= bg->len ? fg->len : bg->len;
        size_t smaller_len = fg->len >= bg->len ? bg->len : fg->len;
        bool fg_larger = fg->len >= bg->len;
        pairs = malloc(larger_len * sizeof(ColorPair));
        pairs_len = larger_len;
        size_t repeat_factor = smaller_len ? larger_len / smaller_len : 0;
        size_t overflow_count = smaller_len ? larger_len % smaller_len : 0;
        bool overflow_used = false;
        size_t smaller_index = 0;
        size_t current_repeat_factor = 0;
        for (size_t i = 0; i < larger_len; i++) {
            if (current_repeat_factor >= repeat_factor) {
                if (overflow_count > 0) {
                    if (overflow_used) {
                        smaller_index++;
                        current_repeat_factor = 0;
                        overflow_used = false;
                    } else {
                        overflow_used = true;
                        overflow_count--;
                    }
                } else {
                    smaller_index++;
                    current_repeat_factor = 0;
                }
            }
            current_repeat_factor++;
            if (smaller_index >= smaller_len) {
                smaller_index = smaller_len ? smaller_len - 1 : 0;
            }
            ColorPair cp;
            memset(&cp, 0, sizeof(cp));
            if (fg_larger) {
                cp.has_fg = true;
                cp.fg = larger[i];
                cp.has_bg = true;
                cp.bg = smaller[smaller_index];
            } else {
                cp.has_fg = true;
                cp.fg = smaller[smaller_index];
                cp.has_bg = true;
                cp.bg = larger[i];
            }
            pairs[i] = cp;
        }
    } else if (fg_has) {
        pairs = malloc(fg->len * sizeof(ColorPair));
        pairs_len = fg->len;
        for (size_t i = 0; i < fg->len; i++) {
            ColorPair cp;
            memset(&cp, 0, sizeof(cp));
            cp.has_fg = true;
            cp.fg = fg->spectrum[i];
            pairs[i] = cp;
        }
    } else {
        pairs = malloc(bg->len * sizeof(ColorPair));
        pairs_len = bg->len;
        for (size_t i = 0; i < bg->len; i++) {
            ColorPair cp;
            memset(&cp, 0, sizeof(cp));
            cp.has_bg = true;
            cp.bg = bg->spectrum[i];
            pairs[i] = cp;
        }
    }

    int rc = 0;
    size_t repeat_factor = pairs_len ? n_symbols / pairs_len : 0;
    size_t overflow_count = pairs_len ? n_symbols % pairs_len : 0;
    bool overflow_used = false;
    size_t smaller_index = 0;
    size_t current_repeat_factor = 0;
    if (n_symbols >= pairs_len) {
        for (size_t i = 0; i < n_symbols; i++) {
            if (current_repeat_factor >= repeat_factor) {
                if (overflow_count > 0) {
                    if (overflow_used) {
                        smaller_index++;
                        current_repeat_factor = 0;
                        overflow_used = false;
                    } else {
                        overflow_used = true;
                        overflow_count--;
                    }
                } else {
                    smaller_index++;
                    current_repeat_factor = 0;
                }
            }
            current_repeat_factor++;
            if (pairs_len && smaller_index >= pairs_len) {
                smaller_index = pairs_len - 1;
            }
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors = pairs[smaller_index];
            if (scene_add_frame(scene, symbols[i], duration, &vp) != 0) {
                rc = -1;
                break;
            }
        }
    } else {
        size_t repeat_factor2 = n_symbols ? pairs_len / n_symbols : 0;
        size_t overflow_count2 = n_symbols ? pairs_len % n_symbols : 0;
        bool overflow_used2 = false;
        size_t smaller_index2 = 0;
        size_t current_repeat_factor2 = 0;
        for (size_t i = 0; i < pairs_len; i++) {
            if (current_repeat_factor2 >= repeat_factor2) {
                if (overflow_count2 > 0) {
                    if (overflow_used2) {
                        smaller_index2++;
                        current_repeat_factor2 = 0;
                        overflow_used2 = false;
                    } else {
                        overflow_used2 = true;
                        overflow_count2--;
                    }
                } else {
                    smaller_index2++;
                    current_repeat_factor2 = 0;
                }
            }
            current_repeat_factor2++;
            if (n_symbols && smaller_index2 >= n_symbols) {
                smaller_index2 = n_symbols - 1;
            }
            VisualParams vp;
            memset(&vp, 0, sizeof(vp));
            vp.has_colors = true;
            vp.colors = pairs[i];
            if (scene_add_frame(scene, symbols[smaller_index2], duration, &vp) != 0) {
                rc = -1;
                break;
            }
        }
    }
    free(pairs);
    return rc;
}

// --- Animation ------------------------------------------------------------

void animation_init(Animation *anim, const char *input_symbol) {
    memset(anim, 0, sizeof(*anim));
    om_init(&anim->scenes);
    anim->input_symbol = dup_cstr(input_symbol);
    anim->existing_color_handling = EXISTING_COLOR_IGNORE;
    vis_init_plain(&anim->current_visual, input_symbol);
}

void animation_free(Animation *anim) {
    for (size_t i = 0; i < om_len(&anim->scenes); i++) {
        scene_free((Scene *)om_value_at(&anim->scenes, i));
    }
    om_free(&anim->scenes);
    free(anim->input_symbol);
    free(anim->active_scene);
    vis_free(&anim->current_visual);
    memset(anim, 0, sizeof(*anim));
}

void animation_set_appearance(Animation *anim, bool uses_input_preexisting_colors, const char *symbol,
                              const ColorPair *colors) {
    const char *use_symbol = symbol ? symbol : anim->input_symbol;
    ColorPair effective;
    if (colors) {
        effective = *colors;
    } else {
        memset(&effective, 0, sizeof(effective));
    }
    bool bold = false;
    if (anim->existing_color_handling == EXISTING_COLOR_ALWAYS && uses_input_preexisting_colors) {
        effective.has_fg = anim->has_input_fg;
        effective.fg = anim->input_fg_color;
        effective.has_bg = anim->has_input_bg;
        effective.bg = anim->input_bg_color;
        bold = anim->input_bold;
    }

    VisualParams params;
    memset(&params, 0, sizeof(params));
    params.bold = bold;
    params.has_colors = true;
    params.colors = effective;
    if (effective.has_fg) {
        params.has_fg_code =
            resolve_color_code(true, &effective.fg, anim->no_color, anim->use_xterm_colors, &params.fg_code);
    }
    if (effective.has_bg) {
        params.has_bg_code =
            resolve_color_code(true, &effective.bg, anim->no_color, anim->use_xterm_colors, &params.bg_code);
    }
    free(anim->current_visual.symbol);
    free(anim->current_visual.formatted);
    anim->current_visual.symbol = NULL;
    anim->current_visual.formatted = NULL;
    vis_init(&anim->current_visual, use_symbol, &params);
}

const char *animation_new_scene(Animation *anim, bool is_looping, bool has_sync, SyncMetric sync, bool has_ease,
                                Easing ease, const char *scene_id, bool uses_input_preexisting_colors) {
    char auto_id[32];
    if (scene_id == NULL || scene_id[0] == '\0') {
        size_t current_id = om_len(&anim->scenes);
        for (;;) {
            snprintf(auto_id, sizeof(auto_id), "%zu", current_id);
            if (!om_contains(&anim->scenes, auto_id)) {
                break;
            }
            current_id++;
        }
        scene_id = auto_id;
    }
    Scene *scene = scene_new(scene_id, is_looping, has_sync, sync, has_ease, ease, anim->no_color,
                             anim->use_xterm_colors);
    if (!scene) {
        return NULL;
    }
    if (anim->existing_color_handling == EXISTING_COLOR_ALWAYS && uses_input_preexisting_colors) {
        scene->has_preexisting_colors = true;
        scene->preexisting_colors.has_fg = anim->has_input_fg;
        scene->preexisting_colors.fg = anim->input_fg_color;
        scene->preexisting_colors.has_bg = anim->has_input_bg;
        scene->preexisting_colors.bg = anim->input_bg_color;
        scene->preexisting_bold = anim->input_bold;
    }
    om_insert(&anim->scenes, scene_id, scene);
    return om_key_at(&anim->scenes, (size_t)om_slot(&anim->scenes, scene_id));
}

bool animation_active_scene_is_complete(const Animation *anim) {
    if (!anim->active_scene) {
        return true;
    }
    Scene *scene = (Scene *)om_get(&anim->scenes, anim->active_scene);
    if (!scene) {
        return true;
    }
    return iq_len(&scene->frames) == 0 || scene->is_looping;
}

Scene *animation_active_scene(Animation *anim) {
    if (!anim->active_scene) {
        return NULL;
    }
    return (Scene *)om_get(&anim->scenes, anim->active_scene);
}

void animation_clear_scenes(Animation *anim) {
    for (size_t i = 0; i < om_len(&anim->scenes); i++) {
        scene_free((Scene *)om_value_at(&anim->scenes, i));
    }
    om_clear(&anim->scenes);
}
