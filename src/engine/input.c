#include "engine/input.h"

#include <stdlib.h>
#include <string.h>

#include "engine/terminal.h"
#include "utils/strbuf.h"
#include "utils/utf8.h"

// --- ColorFrequency -------------------------------------------------------

void colorfreq_init(ColorFrequency *freq) {
    freq->items = NULL;
    freq->len = 0;
    freq->cap = 0;
}

void colorfreq_free(ColorFrequency *freq) {
    free(freq->items);
    colorfreq_init(freq);
}

void colorfreq_increment(ColorFrequency *freq, const Color *color) {
    for (size_t i = 0; i < freq->len; i++) {
        if (color_eq(&freq->items[i].color, color)) {
            freq->items[i].count++;
            return;
        }
    }
    if (freq->len == freq->cap) {
        size_t cap = freq->cap ? freq->cap * 2 : 8;
        ColorFreqEntry *grown = realloc(freq->items, cap * sizeof(ColorFreqEntry));
        if (!grown) {
            return;
        }
        freq->items = grown;
        freq->cap = cap;
    }
    freq->items[freq->len].color = *color;
    freq->items[freq->len].count = 1;
    freq->len++;
}

void charidrows_free(CharIdRows *rows) {
    for (size_t i = 0; i < rows->len; i++) {
        free(rows->rows[i].items);
    }
    free(rows->rows);
    rows->rows = NULL;
    rows->len = 0;
}

// --- screen hash map ------------------------------------------------------

typedef struct {
    uint64_t key;
    CharId val;
} ScreenSlot;

typedef struct {
    ScreenSlot *slots;
    size_t cap;
    size_t count;
} ScreenMap;

static void screen_init(ScreenMap *m) {
    m->cap = 16;
    m->count = 0;
    m->slots = malloc(m->cap * sizeof(ScreenSlot));
    for (size_t i = 0; i < m->cap; i++) {
        m->slots[i].key = UINT64_MAX;
    }
}

static void screen_free(ScreenMap *m) {
    free(m->slots);
    m->slots = NULL;
}

static size_t screen_hash(uint64_t key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return (size_t)key;
}

static void screen_insert(ScreenMap *m, uint64_t key, CharId val);

static void screen_grow(ScreenMap *m) {
    size_t old_cap = m->cap;
    ScreenSlot *old = m->slots;
    m->cap = old_cap * 2;
    m->slots = malloc(m->cap * sizeof(ScreenSlot));
    for (size_t i = 0; i < m->cap; i++) {
        m->slots[i].key = UINT64_MAX;
    }
    m->count = 0;
    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].key != UINT64_MAX) {
            screen_insert(m, old[i].key, old[i].val);
        }
    }
    free(old);
}

static void screen_insert(ScreenMap *m, uint64_t key, CharId val) {
    if ((m->count + 1) * 2 >= m->cap) {
        screen_grow(m);
    }
    size_t mask = m->cap - 1;
    size_t i = screen_hash(key) & mask;
    while (m->slots[i].key != UINT64_MAX && m->slots[i].key != key) {
        i = (i + 1) & mask;
    }
    if (m->slots[i].key == UINT64_MAX) {
        m->count++;
    }
    m->slots[i].key = key;
    m->slots[i].val = val;
}

static CharId screen_get(const ScreenMap *m, uint64_t key) {
    size_t mask = m->cap - 1;
    size_t i = screen_hash(key) & mask;
    while (m->slots[i].key != UINT64_MAX) {
        if (m->slots[i].key == key) {
            return m->slots[i].val;
        }
        i = (i + 1) & mask;
    }
    return CHAR_ID_NONE;
}

static uint64_t coord_key(int64_t row, int64_t col) {
    return ((uint64_t)row << 32) | (uint32_t)col;
}

// --- active SGR state -----------------------------------------------------

typedef struct {
    char fg_sequence[64];  // empty = none, like the reference active_sequences
    char bg_sequence[64];
    bool has_fg_color;
    Color fg_color;
    bool has_bg_color;
    Color bg_color;
    bool bold;
    bool has_standard_fg;
    int64_t standard_fg_parameter;
} ActiveState;

static void active_state_default(ActiveState *s) {
    memset(s, 0, sizeof(*s));
}

// --- preprocessor ---------------------------------------------------------

static int err_unsupported(PreprocessError *err, const char *sequence) {
    err->status = PREPROCESS_UNSUPPORTED_ANSI;
    snprintf(err->sequence, sizeof(err->sequence), "%s", sequence);
    return -1;
}

static int err_invalid_color(PreprocessError *err, const char *message) {
    err->status = PREPROCESS_INVALID_COLOR;
    snprintf(err->message, sizeof(err->message), "%s", message);
    return -1;
}

static CharId build_character(Arena *arena, uint32_t *next_character_id, ColorFrequency *freq,
                              const TerminalConfig *config, const char *symbol, const ActiveState *state,
                              PreprocessError *err) {
    CharId id = arena_alloc(arena);
    if (id == CHAR_ID_NONE) {
        err_invalid_color(err, "out of memory");
        return CHAR_ID_NONE;
    }
    EffectCharacter *ch = &arena->items[id];
    character_init(ch, *next_character_id, symbol, 0, 0);
    (*next_character_id)++;

    if (state->fg_sequence[0] != '\0' && state->has_fg_color) {
        ch->has_input_fg_seq = true;
        ch->input_ansi_fg_sequence = malloc(strlen(state->fg_sequence) + 1);
        if (ch->input_ansi_fg_sequence) {
            strcpy(ch->input_ansi_fg_sequence, state->fg_sequence);
        }
        colorfreq_increment(freq, &state->fg_color);
        ch->animation.has_input_fg = true;
        ch->animation.input_fg_color = state->fg_color;
    }
    if (state->bg_sequence[0] != '\0' && state->has_bg_color) {
        ch->has_input_bg_seq = true;
        ch->input_ansi_bg_sequence = malloc(strlen(state->bg_sequence) + 1);
        if (ch->input_ansi_bg_sequence) {
            strcpy(ch->input_ansi_bg_sequence, state->bg_sequence);
        }
        colorfreq_increment(freq, &state->bg_color);
        ch->animation.has_input_bg = true;
        ch->animation.input_bg_color = state->bg_color;
    }
    ch->animation.input_bold = state->bold;
    ch->animation.no_color = config->no_color;
    ch->animation.use_xterm_colors = config->xterm_colors;
    ch->animation.existing_color_handling = config->existing_color_handling;
    ch->uses_input_preexisting_colors = true;
    if (ch->animation.existing_color_handling == EXISTING_COLOR_ALWAYS) {
        animation_set_appearance(&ch->animation, true, NULL, NULL);
    }
    return id;
}

static int parse_csi_parameters(const char *text, int64_t **out, size_t *out_n) {
    for (const char *p = text; *p; p++) {
        if (!((*p >= '0' && *p <= '9') || *p == ';')) {
            return -1;
        }
    }
    if (*text == '\0') {
        *out = NULL;
        *out_n = 0;
        return 0;
    }
    size_t count = 1;
    for (const char *p = text; *p; p++) {
        if (*p == ';') {
            count++;
        }
    }
    int64_t *params = malloc(count * sizeof(int64_t));
    size_t idx = 0;
    const char *p = text;
    while (p) {
        const char *semi = strchr(p, ';');
        size_t len = semi ? (size_t)(semi - p) : strlen(p);
        if (len == 0) {
            params[idx++] = 0;
        } else {
            char buf[32];
            if (len >= sizeof(buf)) {
                free(params);
                return -1;
            }
            memcpy(buf, p, len);
            buf[len] = '\0';
            params[idx++] = (int64_t)strtoll(buf, NULL, 10);
        }
        p = semi ? semi + 1 : NULL;
    }
    *out = params;
    *out_n = idx;
    return 0;
}

static int xterm_color(int64_t code, Color *out, PreprocessError *err) {
    if (code >= 0 && code <= 255) {
        *out = color_from_xterm((uint8_t)code);
        return 0;
    }
    char msg[64];
    snprintf(msg, sizeof(msg), "invalid xterm color code in input: %lld", (long long)code);
    err_invalid_color(err, msg);
    return -1;
}

static int apply_sgr_sequence(const char *sequence, const char *params_text, ActiveState *state,
                              PreprocessError *err) {
    int64_t *parameters = NULL;
    size_t n = 0;
    if (parse_csi_parameters(params_text, &parameters, &n) != 0) {
        return err_unsupported(err, sequence);
    }
    int64_t single_zero[1] = {0};
    if (n == 0) {
        parameters = single_zero;
        n = 1;
    }
    size_t idx = 0;
    int rc = 0;
    while (idx < n) {
        int64_t parameter = parameters[idx];
        switch (parameter) {
            case 0:
                state->fg_sequence[0] = '\0';
                state->bg_sequence[0] = '\0';
                state->has_fg_color = false;
                state->has_bg_color = false;
                state->bold = false;
                state->has_standard_fg = false;
                break;
            case 1:
                state->bold = true;
                if (state->has_standard_fg) {
                    Color c;
                    if (xterm_color(state->standard_fg_parameter - 30 + 8, &c, err) != 0) {
                        rc = -1;
                        goto done;
                    }
                    state->fg_color = c;
                    state->has_fg_color = true;
                }
                break;
            case 22:
                state->bold = false;
                if (state->has_standard_fg) {
                    Color c;
                    if (xterm_color(state->standard_fg_parameter - 30, &c, err) != 0) {
                        rc = -1;
                        goto done;
                    }
                    state->fg_color = c;
                    state->has_fg_color = true;
                }
                break;
            case 39:
                state->fg_sequence[0] = '\0';
                state->has_fg_color = false;
                state->has_standard_fg = false;
                break;
            case 49:
                state->bg_sequence[0] = '\0';
                state->has_bg_color = false;
                break;
            default:
                if (parameter >= 30 && parameter <= 37) {
                    int64_t code = parameter - 30 + (state->bold ? 8 : 0);
                    Color c;
                    if (xterm_color(code, &c, err) != 0) {
                        rc = -1;
                        goto done;
                    }
                    snprintf(state->fg_sequence, sizeof(state->fg_sequence), "\x1b[%lldm", (long long)parameter);
                    state->fg_color = c;
                    state->has_fg_color = true;
                    state->has_standard_fg = true;
                    state->standard_fg_parameter = parameter;
                } else if (parameter >= 90 && parameter <= 97) {
                    Color c;
                    if (xterm_color(parameter - 90 + 8, &c, err) != 0) {
                        rc = -1;
                        goto done;
                    }
                    snprintf(state->fg_sequence, sizeof(state->fg_sequence), "\x1b[%lldm", (long long)parameter);
                    state->fg_color = c;
                    state->has_fg_color = true;
                    state->has_standard_fg = false;
                } else if (parameter >= 40 && parameter <= 47) {
                    Color c;
                    if (xterm_color(parameter - 40, &c, err) != 0) {
                        rc = -1;
                        goto done;
                    }
                    snprintf(state->bg_sequence, sizeof(state->bg_sequence), "\x1b[%lldm", (long long)parameter);
                    state->bg_color = c;
                    state->has_bg_color = true;
                } else if (parameter >= 100 && parameter <= 107) {
                    Color c;
                    if (xterm_color(parameter - 100 + 8, &c, err) != 0) {
                        rc = -1;
                        goto done;
                    }
                    snprintf(state->bg_sequence, sizeof(state->bg_sequence), "\x1b[%lldm", (long long)parameter);
                    state->bg_color = c;
                    state->has_bg_color = true;
                } else if (parameter == 38 || parameter == 48) {
                    if (idx + 1 >= n) {
                        rc = err_unsupported(err, sequence);
                        goto done;
                    }
                    bool is_fg = (parameter == 38);
                    int64_t selector = parameter;
                    int64_t color_mode = parameters[idx + 1];
                    char normalized[64];
                    Color color;
                    if (color_mode == 5) {
                        if (idx + 2 >= n) {
                            rc = err_unsupported(err, sequence);
                            goto done;
                        }
                        int64_t code = parameters[idx + 2];
                        if (xterm_color(code, &color, err) != 0) {
                            rc = -1;
                            goto done;
                        }
                        idx += 2;
                        snprintf(normalized, sizeof(normalized), "\x1b[%lld;5;%lldm", (long long)selector,
                                 (long long)code);
                    } else if (color_mode == 2) {
                        if (idx + 4 >= n) {
                            rc = err_unsupported(err, sequence);
                            goto done;
                        }
                        char hex[64];
                        size_t hlen = 0;
                        for (size_t o = 2; o < 5; o++) {
                            char piece[24];
                            int pn = snprintf(piece, sizeof(piece), "%02llX",
                                              (unsigned long long)parameters[idx + o]);
                            if (pn < 0 || hlen + (size_t)pn + 1 > sizeof(hex)) {
                                rc = err_unsupported(err, sequence);
                                goto done;
                            }
                            memcpy(hex + hlen, piece, (size_t)pn);
                            hlen += (size_t)pn;
                        }
                        hex[hlen] = '\0';
                        if (color_from_hex(hex, &color) != 0) {
                            rc = err_invalid_color(err, "invalid color value in input");
                            goto done;
                        }
                        idx += 4;
                        snprintf(normalized, sizeof(normalized), "\x1b[%lld;2;%u;%u;%um", (long long)selector,
                                 color.rgb[0], color.rgb[1], color.rgb[2]);
                    } else {
                        rc = err_unsupported(err, sequence);
                        goto done;
                    }
                    if (is_fg) {
                        snprintf(state->fg_sequence, sizeof(state->fg_sequence), "%s", normalized);
                        state->fg_color = color;
                        state->has_fg_color = true;
                        state->has_standard_fg = false;
                    } else {
                        snprintf(state->bg_sequence, sizeof(state->bg_sequence), "%s", normalized);
                        state->bg_color = color;
                        state->has_bg_color = true;
                    }
                }
                // Any other SGR parameter value is silently ignored (faithful).
                break;
        }
        idx++;
    }
done:
    if (parameters != single_zero) {
        free(parameters);
    }
    return rc;
}

static bool is_supported_private_mode_sequence(const char *sequence) {
    return strcmp(sequence, "\x1b[?25h") == 0 || strcmp(sequence, "\x1b[?25l") == 0 ||
           strcmp(sequence, "\x1b[?7h") == 0 || strcmp(sequence, "\x1b[?7l") == 0;
}

static int64_t default_parameter(const int64_t *params, size_t n) {
    if (n == 0) {
        return 1;
    }
    return params[0] > 1 ? params[0] : 1;
}

static int apply_cursor_sequence(const char *sequence, const char *params_text, const char *intermediates,
                                 char final_byte, int64_t row, int64_t column, PreprocessError *err,
                                 int64_t *out_row, int64_t *out_col) {
    if (intermediates[0] != '\0') {
        return err_unsupported(err, sequence);
    }
    if (params_text[0] == '?') {
        return err_unsupported(err, sequence);
    }
    int64_t *parameters = NULL;
    size_t n = 0;
    if (parse_csi_parameters(params_text, &parameters, &n) != 0) {
        return err_unsupported(err, sequence);
    }
    switch (final_byte) {
        case 'A':
            row -= default_parameter(parameters, n);
            break;
        case 'B':
            row += default_parameter(parameters, n);
            break;
        case 'C':
            column += default_parameter(parameters, n);
            break;
        case 'D':
            column -= default_parameter(parameters, n);
            break;
        case 'E':
            row += default_parameter(parameters, n);
            column = 0;
            break;
        case 'F':
            row -= default_parameter(parameters, n);
            column = 0;
            break;
        case 'G':
            column = default_parameter(parameters, n) - 1;
            break;
        case 'H':
        case 'f':
            row = default_parameter(parameters, n) - 1;
            if (n >= 2 && parameters[1] != 0) {
                column = parameters[1] - 1;
            } else {
                column = 0;
            }
            break;
        default:
            free(parameters);
            return err_unsupported(err, sequence);
    }
    free(parameters);
    *out_row = row < 0 ? 0 : row;
    *out_col = column < 0 ? 0 : column;
    return 0;
}

// Returns the exclusive end index of the escape sequence match, or -1.
static int64_t match_escape_sequence(const uint32_t *chars, size_t n, size_t start) {
    if (start + 1 < n && chars[start + 1] == ']') {
        size_t run_start = start + 2;
        size_t t = run_start;
        while (t < n && chars[t] != 0x07) {
            t++;
        }
        if (t < n) {
            return (int64_t)(t + 1);
        }
        // No BEL: backtrack for the rightmost ESC\ terminator in the run.
        size_t p = n;
        while (p >= run_start + 2) {
            if (chars[p - 2] == 0x1b && chars[p - 1] == '\\') {
                return (int64_t)p;
            }
            p--;
        }
    }
    if (start + 1 < n && chars[start + 1] == '[') {
        size_t t = start + 2;
        while (t < n && chars[t] >= 0x30 && chars[t] <= 0x3f) {
            t++;
        }
        while (t < n && chars[t] >= 0x20 && chars[t] <= 0x2f) {
            t++;
        }
        if (t < n && chars[t] >= 0x40 && chars[t] <= 0x7e) {
            return (int64_t)(t + 1);
        }
    }
    if (start + 1 < n && chars[start + 1] != '\n') {
        return (int64_t)(start + 2);
    }
    return -1;
}

// Splits a full CSI sequence into params/intermediates/final. Returns false if
// it is not a well-formed CSI sequence.
static bool split_csi(const char *sequence, size_t len, char *params, size_t params_cap, char *inter,
                     size_t inter_cap, char *final_byte) {
    if (len < 3 || sequence[0] != '\x1b' || sequence[1] != '[') {
        return false;
    }
    size_t t = 2;
    size_t params_start = t;
    while (t < len && sequence[t] >= 0x30 && sequence[t] <= 0x3f) {
        t++;
    }
    size_t params_len = t - params_start;
    if (params_len >= params_cap) {
        return false;
    }
    memcpy(params, sequence + params_start, params_len);
    params[params_len] = '\0';
    size_t inter_start = t;
    while (t < len && sequence[t] >= 0x20 && sequence[t] <= 0x2f) {
        t++;
    }
    size_t inter_len = t - inter_start;
    if (inter_len >= inter_cap) {
        return false;
    }
    memcpy(inter, sequence + inter_start, inter_len);
    inter[inter_len] = '\0';
    if (t != len - 1) {
        return false;
    }
    char fb = sequence[t];
    if (!(fb >= 0x40 && fb <= 0x7e)) {
        return false;
    }
    *final_byte = fb;
    return true;
}

int preprocess_input(Arena *arena, uint32_t *next_character_id, ColorFrequency *freq, const TerminalConfig *config,
                     const char *input_data, CharIdRows *out, PreprocessError *err) {
    err->status = PREPROCESS_OK;
    err->message[0] = '\0';
    err->sequence[0] = '\0';

    uint32_t *chars = NULL;
    size_t nchars = 0;
    if (utf8_decode_strict(input_data, strlen(input_data), &chars, &nchars, NULL) != 0) {
        return err_invalid_color(err, "invalid UTF-8 input");
    }

    ScreenMap screen;
    screen_init(&screen);
    ActiveState state;
    active_state_default(&state);
    ActiveState empty_state;
    active_state_default(&empty_state);

    int64_t row = 0;
    int64_t column = 0;
    int64_t max_row = 0;
    int64_t max_column = 0;
    size_t i = 0;
    int rc = 0;
    StrBuf seq;

    while (i < nchars) {
        uint32_t c = chars[i];
        if (c == 0x1b) {
            int64_t end = match_escape_sequence(chars, nchars, i);
            if (end < 0) {
                sb_init(&seq);
                sb_append_cp(&seq, c);
                rc = err_unsupported(err, seq.data ? seq.data : "");
                sb_free(&seq);
                goto cleanup;
            }
            sb_init(&seq);
            for (size_t k = i; k < (size_t)end; k++) {
                sb_append_cp(&seq, chars[k]);
            }
            const char *sequence = seq.data ? seq.data : "";
            size_t seq_len = strlen(sequence);
            if (sequence[0] == '\x1b' && sequence[1] == '[') {
                char params[64];
                char inter[64];
                char final_byte = 0;
                if (!split_csi(sequence, seq_len, params, sizeof(params), inter, sizeof(inter), &final_byte)) {
                    rc = err_unsupported(err, sequence);
                    sb_free(&seq);
                    goto cleanup;
                }
                if (final_byte == 'm') {
                    rc = apply_sgr_sequence(sequence, params, &state, err);
                    sb_free(&seq);
                    if (rc != 0) {
                        goto cleanup;
                    }
                } else if (is_supported_private_mode_sequence(sequence)) {
                    // ignored: cursor show/hide, autowrap on/off
                    sb_free(&seq);
                } else {
                    int64_t new_row = 0;
                    int64_t new_col = 0;
                    rc = apply_cursor_sequence(sequence, params, inter, final_byte, row, column, err, &new_row,
                                               &new_col);
                    sb_free(&seq);
                    if (rc != 0) {
                        goto cleanup;
                    }
                    row = new_row;
                    column = new_col;
                    if (row > max_row) max_row = row;
                    if (column > max_column) max_column = column;
                }
            } else {
                rc = err_unsupported(err, sequence);
                sb_free(&seq);
                goto cleanup;
            }
            i = (size_t)end;
        } else if (c == '\n') {
            row += 1;
            column = 0;
            if (row > max_row) max_row = row;
            i += 1;
        } else if (c == '\r') {
            column = 0;
            i += 1;
        } else {
            char symbol[8];
            int64_t count;
            if (c == '\t') {
                strcpy(symbol, " ");
                int64_t tw = config->tab_width;
                int64_t rem = column % tw;
                count = tw - rem;
            } else {
                symbol[0] = '\0';
                StrBuf tmp;
                sb_init(&tmp);
                sb_append_cp(&tmp, c);
                size_t slen = strlen(tmp.data ? tmp.data : "");
                if (slen >= sizeof(symbol)) {
                    slen = sizeof(symbol) - 1;
                }
                memcpy(symbol, tmp.data ? tmp.data : "", slen);
                symbol[slen] = '\0';
                sb_free(&tmp);
                count = 1;
            }
            for (int64_t k = 0; k < count; k++) {
                CharId id = build_character(arena, next_character_id, freq, config, symbol, &state, err);
                if (id == CHAR_ID_NONE) {
                    rc = -1;
                    goto cleanup;
                }
                screen_insert(&screen, coord_key(row, column), id);
                if (row > max_row) max_row = row;
                if (column > max_column) max_column = column;
                column += 1;
            }
            i += 1;
        }
    }

    // Materialize rows top-first, building space chars for empty cells.
    size_t row_count = (size_t)(max_row + 1);
    size_t col_count = (size_t)(max_column + 1);
    out->rows = calloc(row_count, sizeof(CharIdRow));
    out->len = 0;
    for (size_t r = 0; r < row_count; r++) {
        CharIdRow line;
        line.items = malloc((col_count ? col_count : 1) * sizeof(CharId));
        line.len = 0;
        for (size_t cidx = 0; cidx < col_count; cidx++) {
            CharId id = screen_get(&screen, coord_key((int64_t)r, (int64_t)cidx));
            if (id == CHAR_ID_NONE) {
                id = build_character(arena, next_character_id, freq, config, " ", &empty_state, err);
                if (id == CHAR_ID_NONE) {
                    free(line.items);
                    rc = -1;
                    goto cleanup;
                }
            }
            line.items[line.len++] = id;
        }
        while (line.len > 0) {
            CharId last = line.items[line.len - 1];
            EffectCharacter *ch = &arena->items[last];
            if (strcmp(ch->input_symbol, " ") == 0 && !ch->animation.has_input_fg && !ch->animation.has_input_bg) {
                line.len--;
            } else {
                break;
            }
        }
        out->rows[out->len++] = line;
    }
    while (out->len > 0 && out->rows[out->len - 1].len == 0) {
        free(out->rows[out->len - 1].items);
        out->len--;
    }

    if (out->len == 0) {
        CharId id = build_character(arena, next_character_id, freq, config, " ", &state, err);
        if (id == CHAR_ID_NONE) {
            rc = -1;
            goto cleanup;
        }
        out->rows = realloc(out->rows, sizeof(CharIdRow));
        out->rows[0].items = malloc(sizeof(CharId));
        out->rows[0].items[0] = id;
        out->rows[0].len = 1;
        out->len = 1;
    }

cleanup:
    free(chars);
    screen_free(&screen);
    if (rc != 0) {
        charidrows_free(out);
    }
    return rc;
}
