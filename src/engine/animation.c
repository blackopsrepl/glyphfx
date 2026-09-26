#include "engine/animation.h"

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
        uint8_t code;
        if (color->is_xterm) {
            code = color->xterm;
        } else {
            code = hex_to_xterm(color->hex);
        }
        *out = colorcode_xterm(code);
        return true;
    }
    *out = colorcode_rgb(color->hex);
    return true;
}

static void vis_set_from_params(CharacterVisual *vis, const char *symbol, const VisualParams *params) {
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
    vis->formatted = NULL;
    vis_format(vis);
}

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
    vis_set_from_params(vis, symbol, params);
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

void animation_init(Animation *anim, const char *input_symbol) {
    memset(anim, 0, sizeof(*anim));
    anim->input_symbol = dup_cstr(input_symbol);
    anim->existing_color_handling = EXISTING_COLOR_IGNORE;
    vis_init_plain(&anim->current_visual, input_symbol);
}

void animation_free(Animation *anim) {
    free(anim->input_symbol);
    anim->input_symbol = NULL;
    vis_free(&anim->current_visual);
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
        params.has_fg_code = resolve_color_code(true, &effective.fg, anim->no_color, anim->use_xterm_colors,
                                                 &params.fg_code);
    }
    if (effective.has_bg) {
        params.has_bg_code = resolve_color_code(true, &effective.bg, anim->no_color, anim->use_xterm_colors,
                                                 &params.bg_code);
    }

    free(anim->current_visual.symbol);
    free(anim->current_visual.formatted);
    anim->current_visual.symbol = NULL;
    anim->current_visual.formatted = NULL;
    vis_init(&anim->current_visual, use_symbol, &params);
}
