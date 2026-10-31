#include "engine/terminal.h"
#include "engine/render_log.h"

#include <math.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/ioctl.h>
#endif

#include "utils/pycompat.h"
#include "utils/signals.h"

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#define NOT_VISIBLE SIZE_MAX

static void mirror_fill(Terminal *t, CharId id);

void terminal_config_default(TerminalConfig *config) {
    memset(config, 0, sizeof(*config));
    config->tab_width = 4;
    config->frame_rate = 60;
    config->canvas_width = -1;
    config->canvas_height = -1;
    config->anchor_canvas = ANCHOR_SW;
    config->anchor_text = ANCHOR_SW;
    (void)color_from_hex("000000", &config->terminal_background_color);
    config->existing_color_handling = EXISTING_COLOR_IGNORE;
}

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

// --- coordinate map -------------------------------------------------------

static CharId map_get(const Terminal *t, Coord coord) {
    if (coord.column < 1 || coord.row < 1 || coord.column > t->map_right || coord.row > t->map_top) {
        return CHAR_ID_NONE;
    }
    return t->coord_map[(size_t)coord.row * (size_t)(t->map_right + 1) + (size_t)coord.column];
}

static void map_set(Terminal *t, Coord coord, CharId id) {
    if (coord.column < 1 || coord.row < 1 || coord.column > t->map_right || coord.row > t->map_top) {
        return;
    }
    t->coord_map[(size_t)coord.row * (size_t)(t->map_right + 1) + (size_t)coord.column] = id;
}

// --- layout ---------------------------------------------------------------

static int64_t wrapped_line_count(const int64_t *line_lengths, size_t n, int64_t width) {
    int64_t count = 0;
    for (size_t i = 0; i < n; i++) {
        int64_t remaining = line_lengths[i];
        while (remaining > width) {
            count += 1;
            remaining -= width;
        }
        count += 1;
    }
    return count;
}

static void get_canvas_dimensions(const TerminalConfig *config, const int64_t *line_lengths, size_t n_lines,
                                  int64_t terminal_width, int64_t terminal_height, int64_t *out_height,
                                  int64_t *out_width) {
    int64_t canvas_width;
    if (config->canvas_width > 0) {
        canvas_width = config->canvas_width;
    } else if (config->canvas_width == 0) {
        canvas_width = terminal_width;
    } else {
        int64_t input_width = 0;
        for (size_t i = 0; i < n_lines; i++) {
            if (line_lengths[i] > input_width) {
                input_width = line_lengths[i];
            }
        }
        canvas_width = config->ignore_terminal_dimensions ? input_width
                                                          : (terminal_width < input_width ? terminal_width : input_width);
    }
    int64_t canvas_height;
    if (config->canvas_height > 0) {
        canvas_height = config->canvas_height;
    } else if (config->canvas_height == 0) {
        canvas_height = terminal_height;
    } else {
        int64_t input_height = (int64_t)n_lines;
        if (config->ignore_terminal_dimensions) {
            canvas_height = input_height;
        } else if (config->wrap_text) {
            int64_t wrapped = wrapped_line_count(line_lengths, n_lines, canvas_width);
            canvas_height = terminal_height < wrapped ? terminal_height : wrapped;
        } else {
            canvas_height = terminal_height < input_height ? terminal_height : input_height;
        }
    }
    *out_height = canvas_height;
    *out_width = canvas_width;
}

static void calc_canvas_offsets(const TerminalConfig *config, const Canvas *canvas, int64_t terminal_width,
                                int64_t terminal_height, int64_t *out_col, int64_t *out_row) {
    int64_t column_offset = 0;
    int64_t row_offset = 0;
    switch (config->anchor_canvas) {
        case ANCHOR_S:
        case ANCHOR_N:
        case ANCHOR_C:
            column_offset = py_floor_div(terminal_width, 2) - py_floor_div(canvas->width, 2);
            break;
        case ANCHOR_SE:
        case ANCHOR_E:
        case ANCHOR_NE:
            column_offset = terminal_width - canvas->width;
            break;
        default:
            break;
    }
    switch (config->anchor_canvas) {
        case ANCHOR_W:
        case ANCHOR_E:
        case ANCHOR_C:
            row_offset = py_floor_div(terminal_height, 2) - py_floor_div(canvas->height, 2);
            break;
        case ANCHOR_NW:
        case ANCHOR_N:
        case ANCHOR_NE:
            row_offset = terminal_height - canvas->height;
            break;
        default:
            break;
    }
    *out_col = column_offset;
    *out_row = row_offset;
}

static Layout compute_layout(const TerminalConfig *config, const int64_t *line_lengths, size_t n_lines,
                             int64_t terminal_width, int64_t terminal_height) {
    Layout layout;
    get_canvas_dimensions(config, line_lengths, n_lines, terminal_width, terminal_height,
                          &layout.canvas_height, &layout.canvas_width);
    Canvas canvas = canvas_new(layout.canvas_height, layout.canvas_width);
    int64_t width = terminal_width;
    int64_t height = terminal_height;
    int64_t col_off = 0;
    int64_t row_off = 0;
    if (!config->ignore_terminal_dimensions) {
        calc_canvas_offsets(config, &canvas, width, height, &col_off, &row_off);
    } else {
        width = canvas.right;
        height = canvas.top;
    }
    layout.column_offset = col_off;
    layout.row_offset = row_off;
    layout.visible_top = (canvas.top + row_off) < height ? (canvas.top + row_off) : height;
    layout.visible_bottom = (canvas.bottom + row_off) > 1 ? (canvas.bottom + row_off) : 1;
    layout.visible_right = (canvas.right + col_off) < width ? (canvas.right + col_off) : width;
    layout.visible_left = (canvas.left + col_off) > 1 ? (canvas.left + col_off) : 1;
    return layout;
}

// --- input character setup ------------------------------------------------

static CharIdRows wrap_lines(CharIdRows *in, int64_t width) {
    CharIdRows out;
    out.rows = NULL;
    out.len = 0;
    size_t cap = 0;
    for (size_t i = 0; i < in->len; i++) {
        CharIdRow src = in->rows[i];
        size_t start = 0;
        while ((int64_t)(src.len - start) > width) {
            if (out.len == cap) {
                cap = cap ? cap * 2 : 8;
                out.rows = realloc(out.rows, cap * sizeof(CharIdRow));
            }
            CharIdRow piece;
            piece.len = (size_t)width;
            piece.items = malloc(piece.len * sizeof(CharId));
            memcpy(piece.items, src.items + start, piece.len * sizeof(CharId));
            out.rows[out.len++] = piece;
            start += (size_t)width;
        }
        if (out.len == cap) {
            cap = cap ? cap * 2 : 8;
            out.rows = realloc(out.rows, cap * sizeof(CharIdRow));
        }
        CharIdRow tail;
        tail.len = src.len - start;
        tail.items = malloc((tail.len ? tail.len : 1) * sizeof(CharId));
        memcpy(tail.items, src.items + start, tail.len * sizeof(CharId));
        out.rows[out.len++] = tail;
    }
    return out;
}

// Returns the anchored input characters (malloc'd). On failure returns NULL.
static CharId *setup_input_characters(const TerminalConfig *config, Canvas *canvas, Arena *arena, CharIdRows *lines,
                                      size_t *out_n) {
    CharIdRows formatted = *lines;
    bool wrapped_alloc = false;
    if (config->wrap_text) {
        formatted = wrap_lines(lines, canvas->right);
        wrapped_alloc = true;
    }
    int64_t input_height = (int64_t)formatted.len;
    size_t total_chars = 0;
    for (size_t r = 0; r < formatted.len; r++) {
        total_chars += formatted.rows[r].len;
    }
    CharId *input_characters = malloc((total_chars ? total_chars : 1) * sizeof(CharId));
    size_t n = 0;
    for (size_t r = 0; r < formatted.len; r++) {
        CharIdRow *line = &formatted.rows[r];
        for (size_t c = 0; c < line->len; c++) {
            CharId id = line->items[c];
            EffectCharacter *ch = &arena->items[id];
            ch->input_coord = coord_new((int64_t)c + 1, input_height - (int64_t)r);
            if (strcmp(ch->input_symbol, " ") != 0 || ch->animation.has_input_fg || ch->animation.has_input_bg) {
                input_characters[n++] = id;
            }
        }
    }
    if (wrapped_alloc) {
        for (size_t r = 0; r < formatted.len; r++) {
            free(formatted.rows[r].items);
        }
        free(formatted.rows);
    }
    CharId *kept = NULL;
    size_t kept_n = 0;
    if (canvas_anchor_text(canvas, arena, input_characters, n, config->anchor_text, &kept, &kept_n) != 0) {
        free(input_characters);
        return NULL;
    }
    free(input_characters);
    *out_n = kept_n;
    return kept;
}

// --- Terminal -------------------------------------------------------------

void terminal_get_dimensions(int64_t *width, int64_t *height) {
    const char *env_c = getenv("COLUMNS");
    const char *env_l = getenv("LINES");
    bool has_c = false;
    bool has_l = false;
    int64_t col_env = 0;
    int64_t line_env = 0;
    if (env_c) {
        char *endp = NULL;
        long long v = strtoll(env_c, &endp, 10);
        if (endp && *endp == '\0') {
            col_env = (int64_t)v;
            has_c = true;
        }
    }
    if (env_l) {
        char *endp = NULL;
        long long v = strtoll(env_l, &endp, 10);
        if (endp && *endp == '\0') {
            line_env = (int64_t)v;
            has_l = true;
        }
    }
    if (has_c && has_l) {
        *width = col_env;
        *height = line_env;
        return;
    }
    int64_t qw = 0;
    int64_t qh = 0;
    bool queried = false;
#if defined(TIOCGWINSZ)
    int fds[3] = {STDOUT_FILENO, STDERR_FILENO, STDIN_FILENO};
    for (int i = 0; i < 3; i++) {
        struct winsize ws;
        if (ioctl(fds[i], TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
            qw = ws.ws_col;
            qh = ws.ws_row;
            queried = true;
            break;
        }
    }
#endif
    if (queried) {
        *width = has_c ? col_env : qw;
        *height = has_l ? line_env : qh;
    } else {
        *width = has_c ? col_env : 80;
        *height = has_l ? line_env : 24;
    }
}

int terminal_new(Terminal *t, const char *input_data, const TerminalConfig *config, PreprocessError *err) {
    memset(t, 0, sizeof(*t));
    t->config = *config;
    t->frame_rate = config->frame_rate;
    arena_init(&t->arena);
    colorfreq_init(&t->input_colors_frequency);
    t->next_character_id = 0;

    const char *data = input_data[0] == '\0' ? "No Input." : input_data;
    CharIdRows lines;
    memset(&lines, 0, sizeof(lines));
    if (preprocess_input(&t->arena, &t->next_character_id, &t->input_colors_frequency, config, data, &lines,
                         err) != 0) {
        arena_free(&t->arena);
        colorfreq_free(&t->input_colors_frequency);
        return -1;
    }

    t->input_line_lengths_len = lines.len;
    t->input_line_lengths = malloc((lines.len ? lines.len : 1) * sizeof(int64_t));
    for (size_t i = 0; i < lines.len; i++) {
        t->input_line_lengths[i] = (int64_t)lines.rows[i].len;
    }

    terminal_get_dimensions(&t->terminal_width, &t->terminal_height);
    t->layout = compute_layout(config, t->input_line_lengths, lines.len, t->terminal_width, t->terminal_height);
    t->canvas = canvas_new(t->layout.canvas_height, t->layout.canvas_width);
    t->canvas_column_offset = t->layout.column_offset;
    t->canvas_row_offset = t->layout.row_offset;
    t->visible_top = t->layout.visible_top;
    t->visible_bottom = t->layout.visible_bottom;
    t->visible_right = t->layout.visible_right;
    t->visible_left = t->layout.visible_left;

    size_t input_n = 0;
    CharId *input_chars = setup_input_characters(config, &t->canvas, &t->arena, &lines, &input_n);
    for (size_t i = 0; i < lines.len; i++) {
        free(lines.rows[i].items);
    }
    free(lines.rows);
    if (!input_chars) {
        arena_free(&t->arena);
        colorfreq_free(&t->input_colors_frequency);
        free(t->input_line_lengths);
        return -1;
    }
    t->input_characters = input_chars;
    t->input_characters_len = input_n;

    // coord map over the canvas
    t->map_right = t->canvas.right;
    t->map_top = t->canvas.top;
    size_t map_cells = (size_t)(t->map_right + 1) * (size_t)(t->map_top + 1);
    t->coord_map = malloc(map_cells * sizeof(CharId));
    for (size_t i = 0; i < map_cells; i++) {
        t->coord_map[i] = CHAR_ID_NONE;
    }
    for (size_t i = 0; i < t->input_characters_len; i++) {
        CharId id = t->input_characters[i];
        Coord c = t->arena.items[id].input_coord;
        if (c.row <= t->canvas.top && c.column <= t->canvas.right) {
            map_set(t, c, id);
        }
    }

    // move_cursor_to_top
    {
        StrBuf sb;
        sb_init(&sb);
        sb_puts(&sb, ANSI_DEC_RESTORE_CURSOR);
        sb_puts(&sb, ANSI_DEC_SAVE_CURSOR);
        ansi_move_cursor_up(&sb, t->visible_top > 0 ? t->visible_top : 0);
        t->move_cursor_to_top = sb_take(&sb);
    }

    // make fill characters
    for (int64_t row = 1; row <= t->canvas.top; row++) {
        for (int64_t column = 1; column <= t->canvas.right; column++) {
            Coord coord = coord_new(column, row);
            if (map_get(t, coord) != CHAR_ID_NONE) {
                continue;
            }
            CharId id = arena_alloc(&t->arena);
            EffectCharacter *fill = &t->arena.items[id];
            character_init(fill, t->next_character_id, " ", column, row);
            t->next_character_id++;
            fill->is_fill_character = true;
            fill->animation.no_color = config->no_color;
            fill->animation.use_xterm_colors = config->xterm_colors;
            fill->animation.existing_color_handling = config->existing_color_handling;
            fill->uses_input_preexisting_colors = false;
            map_set(t, coord, id);
            bool in_text = t->canvas.text_left <= column && column <= t->canvas.text_right &&
                           t->canvas.text_bottom <= row && row <= t->canvas.text_top;
            if (in_text) {
                t->inner_fill_characters =
                    realloc(t->inner_fill_characters, (t->inner_fill_characters_len + 1) * sizeof(CharId));
                t->inner_fill_characters[t->inner_fill_characters_len++] = id;
            } else {
                t->outer_fill_characters =
                    realloc(t->outer_fill_characters, (t->outer_fill_characters_len + 1) * sizeof(CharId));
                t->outer_fill_characters[t->outer_fill_characters_len++] = id;
            }
        }
    }

    // neighbors
    for (int64_t row = 1; row <= t->canvas.top; row++) {
        for (int64_t column = 1; column <= t->canvas.right; column++) {
            CharId id = map_get(t, coord_new(column, row));
            if (id == CHAR_ID_NONE) {
                continue;
            }
            EffectCharacter *ch = &t->arena.items[id];
            ch->north = map_get(t, coord_new(column, row + 1));
            ch->east = map_get(t, coord_new(column + 1, row));
            ch->south = map_get(t, coord_new(column, row - 1));
            ch->west = map_get(t, coord_new(column - 1, row));
        }
    }

    t->last_time_printed = monotonic_seconds();
    t->visible_positions_len = t->arena.len;
    t->visible_positions = malloc((t->arena.len ? t->arena.len : 1) * sizeof(size_t));
    for (size_t i = 0; i < t->visible_positions_len; i++) {
        t->visible_positions[i] = NOT_VISIBLE;
    }
    return 0;
}

void terminal_free(Terminal *t) {
    render_log_release(t);
    arena_free(&t->arena);
    colorfreq_free(&t->input_colors_frequency);
    free(t->input_line_lengths);
    free(t->input_characters);
    free(t->added_characters);
    free(t->coord_map);
    free(t->inner_fill_characters);
    free(t->outer_fill_characters);
    free(t->visible_characters);
    free(t->visible_positions);
    free(t->render_cells);
    free(t->cell_visual);
    free(t->previous_cell_visual);
    free(t->row_dirty);
    free(t->cell_dirty);
    free(t->row_offs);
    free(t->block_bits);
    free(t->cell_head);
    free(t->cell_next);
    free(t->cell_of_char);
    free(t->render_mirror);
    t->render_mirror = NULL;
    t->render_mirror_len = 0;
    for (size_t i = 0; i < t->row_store_len; i++) {
        free(t->row_store[i]);
    }
    free(t->row_store);
    free(t->row_bytes);
    free(t->row_lens);
    free(t->row_caps);
    free(t->row_sel);
    free(t->move_cursor_to_top);
    free(t->frame_iov);
    sb_free(&t->output_buffer);
    memset(t, 0, sizeof(*t));
}

CharId terminal_get_character_by_input_coord(const Terminal *t, Coord coord) {
    return map_get(t, coord);
}

CharId terminal_add_character(Terminal *t, const char *symbol, Coord coord) {
    CharId id = arena_alloc(&t->arena);
    EffectCharacter *ch = &t->arena.items[id];
    character_init(ch, t->next_character_id, symbol, coord.column, coord.row);
    t->next_character_id++;
    ch->animation.no_color = t->config.no_color;
    ch->animation.use_xterm_colors = t->config.xterm_colors;
    ch->animation.existing_color_handling = t->config.existing_color_handling;
    ch->uses_input_preexisting_colors = false;
    // Spare capacity is not initialized storage: a character added below the
    // current capacity will not be visited by grid_grow_chars again.
    if ((size_t)id < t->render_mirror_len) {
        mirror_fill(t, id);
    }
    t->added_characters = realloc(t->added_characters, (t->added_characters_len + 1) * sizeof(CharId));
    t->added_characters[t->added_characters_len++] = id;
    return id;
}

void terminal_set_character_visibility(Terminal *t, CharId id, bool is_visible) {
    size_t index = (size_t)id;
    if (t->arena.items[index].is_visible == is_visible) {
        return;
    }
    t->arena.items[index].is_visible = is_visible;
    if ((size_t)index < t->render_mirror_len) {
        t->render_mirror[index].visible = is_visible;
    }
    renderer_move(id, t->arena.items[index].motion.current_coord);
    if (t->arena.len > t->visible_positions_len) {
        size_t old = t->visible_positions_len;
        t->visible_positions = realloc(t->visible_positions, t->arena.len * sizeof(size_t));
        for (size_t i = old; i < t->arena.len; i++) {
            t->visible_positions[i] = NOT_VISIBLE;
        }
        t->visible_positions_len = t->arena.len;
    }
    if (is_visible) {
        t->visible_positions[index] = t->visible_characters_len;
        t->visible_characters =
            realloc(t->visible_characters, (t->visible_characters_len + 1) * sizeof(CharId));
        t->visible_characters[t->visible_characters_len++] = id;
    } else {
        size_t position = t->visible_positions[index];
        t->visible_positions[index] = NOT_VISIBLE;
        if (position == NOT_VISIBLE) {
            return;
        }
        size_t last = t->visible_characters_len - 1;
        if (position < last) {
            CharId moved = t->visible_characters[last];
            t->visible_characters[position] = moved;
            t->visible_positions[moved] = position;
        }
        t->visible_characters_len--;
    }
}

// Empty cells hold this sentinel; a CharId never collides with it (the arena
// is far smaller than 2^32).
#define RENDER_EMPTY UINT32_MAX

// GLYPHFX_RENDER_VERIFY=1 re-serializes rows the cache deemed clean and aborts
// on a mismatch: a cheap proof that the version grid catches every content
// change, run over the parity suite and full-size workloads.
static bool g_render_verify;

// Clean-row gating: the cache's bookkeeping costs a little every frame, so it
// is only kept on where most rows turn out clean. The asm renderer knows this
// from its change log; here the writer measures the clean fraction.
#define ROW_CACHE_ON_NUM 3
#define ROW_CACHE_ON_DEN 4     // establish the cache above 3/4 clean
#define ROW_CACHE_KEEP_NUM 1
#define ROW_CACHE_KEEP_DEN 2   // keep it above 1/2 clean
#define ROW_CACHE_WINDOW 64
#define ROW_CACHE_PROBE 128          // first probe delay while off
#define ROW_CACHE_PROBE_MAX 2048     // backoff cap keeps settled phases reachable
#define ROW_CACHE_PROBE_MIN 8        // frames before a hopeless probe is abandoned

static void row_cache_ensure(Terminal *t, size_t width, size_t height);

// A cell whose winning handle changed: its dirty byte, and its row's.
static inline void mark_cell(Terminal *t, int64_t cell, size_t width) {
    size_t row = (size_t)cell / width;
    t->cell_dirty[row * t->dirty_stride + ((size_t)cell - row * width)] = 1;
    t->row_dirty[row] = 1;
}

// Set the dirty byte of every cell whose handle differs between the grids,
// 16 cells a compare; returns whether any did.
static bool mark_changed(uint8_t *dirty, const VisualHandle *now, const VisualHandle *was, size_t n) {
    size_t col = 0;
    bool any = false;
#if defined(__SSE2__)
    const __m128i one = _mm_set1_epi8(1);
    unsigned changed = 0;
    for (; col + 16 <= n; col += 16) {
        __m128i e[4];
        for (int i = 0; i < 4; i++) {
            __m128i a = _mm_loadu_si128((const __m128i *)(const void *)(now + col + 4 * i));
            __m128i b = _mm_loadu_si128((const __m128i *)(const void *)(was + col + 4 * i));
            e[i] = _mm_cmpeq_epi32(a, b);
        }
        // 0/-1 lanes pack to 0/-1 bytes: equal cells are 0xFF
        __m128i eq = _mm_packs_epi16(_mm_packs_epi32(e[0], e[1]), _mm_packs_epi32(e[2], e[3]));
        __m128i diff = _mm_andnot_si128(eq, one);
        unsigned m = (unsigned)_mm_movemask_epi8(_mm_cmpeq_epi8(diff, one));
        if (m) {
            __m128i d = _mm_loadu_si128((const __m128i *)(const void *)(dirty + col));
            _mm_storeu_si128((__m128i *)(void *)(dirty + col), _mm_or_si128(d, diff));
            changed |= m;
        }
    }
    any = changed != 0;
#endif
    for (; col < n; col++) {
        if (now[col] != was[col]) {
            dirty[col] = 1;
            any = true;
        }
    }
    return any;
}

// --- incremental grid -----------------------------------------------------

// Painter order: (layer, character_id) maximum wins a contested cell. The
// mirror keeps both next to the coordinate.
static inline bool grid_beats(const Terminal *t, CharId a, CharId b) {
    const RenderMirror *ma = &t->render_mirror[a];
    const RenderMirror *mb = &t->render_mirror[b];
    return ma->layer > mb->layer ||
           (ma->layer == mb->layer && ma->character_id > mb->character_id);
}

// Fill one slot's mirror from the arena record.
static void mirror_fill(Terminal *t, CharId id) {
    EffectCharacter *ch = &t->arena.items[id];
    RenderMirror *m = &t->render_mirror[id];
    m->coord = ch->motion.current_coord;
    m->layer = ch->layer;
    m->character_id = ch->character_id;
    m->vis = ch->animation.current_visual;
    m->visible = ch->is_visible;
}

// Re-picks a cell's owner among its occupants (one left, appeared, or changed
// layer) and refreshes the version the row cache compares.
static void cell_rewin(Terminal *t, int64_t cell, size_t width) {
    (void)width;
    int32_t best = -1;
    for (int32_t o = t->cell_head[cell]; o >= 0; o = t->cell_next[o]) {
        if (best < 0 || grid_beats(t, (CharId)o, (CharId)best)) {
            best = o;
        }
    }
    uint32_t owner = best < 0 ? RENDER_EMPTY : (uint32_t)best;
    if (t->render_cells[cell] == owner) {
        return;
    }
    if (t->render_cells[cell] == RENDER_EMPTY) {
        t->painted_cells++;
    } else if (owner == RENDER_EMPTY) {
        t->painted_cells--;
    }
    t->render_cells[cell] = owner;
    VisualHandle vis = owner == RENDER_EMPTY ? 0 : t->render_mirror[owner].vis;
    if (t->cell_visual[cell] != vis) {
        t->cell_visual[cell] = vis;
        mark_cell(t, cell, width);
    }
}

// MOVE record: leave the old cell (rewinning it when it was owned) and join
// the new one.
static void grid_move(Terminal *t, CharId id, int32_t new_cell, size_t width) {
    int32_t old = t->cell_of_char[id];
    if (old == new_cell) {
        return;
    }
    if (old >= 0) {
        int32_t *link = &t->cell_head[old];
        while (*link != (int32_t)id) {
            link = &t->cell_next[*link];
        }
        *link = t->cell_next[id];
        if (t->render_cells[old] == (uint32_t)id) {
            cell_rewin(t, old, width);
        }
    }
    t->cell_of_char[id] = new_cell;
    if (new_cell >= 0) {
        t->cell_next[id] = t->cell_head[new_cell];
        t->cell_head[new_cell] = (int32_t)id;
        uint32_t *owner = &t->render_cells[new_cell];
        if (*owner == RENDER_EMPTY || grid_beats(t, id, (CharId)*owner)) {
            if (*owner == RENDER_EMPTY) {
                t->painted_cells++;
            }
            *owner = (uint32_t)id;
            VisualHandle vis = t->render_mirror[id].vis;
            if (t->cell_visual[new_cell] != vis) {
                t->cell_visual[new_cell] = vis;
                mark_cell(t, new_cell, width);
            }
        }
    }
}

// Replay the frame's records in order, each carrying the value at the moment
// of the mutation.
static void grid_replay(Terminal *t, size_t width) {
    for (uint32_t i = 0; i < g_rlog.len; i++) {
        uint64_t rec = g_rlog.buf[i];
        uint32_t slot = (uint32_t)(rec & RLOG_SLOT_MASK);
        uint32_t kind = (uint32_t)((rec >> 30) & 3u);
        uint32_t value = (uint32_t)(rec >> 32);
        if ((size_t)slot >= t->arena.len) {
            continue;
        }
        CharId id = (CharId)slot;
        if (kind == RLOG_MOVE) {
            grid_move(t, id, (int32_t)value, width);
        } else if (kind == RLOG_HANDLE) {
            int32_t cell = t->cell_of_char[id];
            if (cell >= 0 && t->render_cells[cell] == slot) {
                VisualHandle vis = t->render_mirror[id].vis;
                if (t->cell_visual[cell] != vis) {
                    t->cell_visual[cell] = vis;
                    mark_cell(t, cell, width);
                }
            }
        } else {
            int32_t cell = t->cell_of_char[id];
            if (cell >= 0) {
                cell_rewin(t, cell, width);
            }
        }
    }
}

// Full repaint from the arena. `lists` also rebuilds the occupant lists and
// per-character mirrors the replay needs.
static void grid_walk(Terminal *t, size_t width, size_t height, bool lists) {
    size_t cell_count = width * height;
    memset(t->render_cells, 0xFF, cell_count * sizeof(uint32_t));
    VisualHandle *previous = t->cell_visual;
    t->cell_visual = t->previous_cell_visual;
    t->previous_cell_visual = previous;
    memset(t->cell_visual, 0, cell_count * sizeof(VisualHandle));
    size_t painted = 0;
    if (lists) {
        memset(t->cell_head, 0xFF, cell_count * sizeof(int32_t));
        for (size_t id = 0; id < t->arena.len; id++) {
            t->cell_of_char[id] = -1;
        }
    }
    for (size_t i = 0; i < t->visible_characters_len; i++) {
        CharId id = t->visible_characters[i];
        const RenderMirror *rec = &t->render_mirror[id];
        int64_t row = rec->coord.row + t->canvas_row_offset;
        int64_t column = rec->coord.column + t->canvas_column_offset;
        if (!(t->visible_bottom <= row && row <= t->visible_top && t->visible_left <= column &&
              column <= t->visible_right)) {
            continue;
        }
        int64_t cell = (row - 1) * (int64_t)width + (column - 1);
        if (lists) {
            t->cell_of_char[id] = (int32_t)cell;
            t->cell_next[id] = t->cell_head[cell];
            t->cell_head[cell] = (int32_t)id;
        }
        uint32_t *owner = &t->render_cells[cell];
        if (*owner == RENDER_EMPTY) {
            *owner = (uint32_t)id;
        } else if (*owner != (uint32_t)id) {
            if (!grid_beats(t, id, (CharId)*owner)) {
                continue;
            }
            *owner = (uint32_t)id;
        }
        t->cell_visual[cell] = rec->vis;
        painted++;
    }
    // Repainting owners does not imply that their bytes changed. Compare
    // contiguous visual rows, including cells that became empty; a changed
    // cell sets its dirty byte for block-level emission.
    for (size_t row = 0; row < height; row++) {
        size_t start = row * width;
        if (mark_changed(t->cell_dirty + row * t->dirty_stride, t->cell_visual + start, previous + start, width)) {
            t->row_dirty[row] = 1;
        }
    }
    t->painted_cells = painted;
    t->grid_ready = true;
}

// Characters may be added mid-run; the per-character arrays are arena-dense.
static void grid_grow_chars(Terminal *t) {
    if (t->arena.len <= t->grid_chars_len) {
        return;
    }
    size_t cap = t->grid_chars_len ? t->grid_chars_len : 64;
    while (cap < t->arena.len) {
        cap *= 2;
    }
    int32_t *next = realloc(t->cell_next, cap * sizeof(int32_t));
    if (!next) {
        return;
    }
    t->cell_next = next;
    int32_t *of = realloc(t->cell_of_char, cap * sizeof(int32_t));
    if (!of) {
        return;
    }
    t->cell_of_char = of;
    RenderMirror *mrec = realloc(t->render_mirror, cap * sizeof(RenderMirror));
    if (mrec) {
        t->render_mirror = mrec;
        t->render_mirror_len = cap;
    }
    for (size_t id = t->grid_chars_len; id < cap; id++) {
        t->cell_next[id] = -1;
        t->cell_of_char[id] = -1;
        if (id < t->arena.len) {
            mirror_fill(t, (CharId)id);
        }
    }
    t->grid_chars_len = cap;
    t->grid_ready = false;
    t->lists_valid = false;
    g_rlog.on = false;
}

static void update_render_cells(Terminal *t, size_t *out_width, size_t *out_height) {
    size_t width = t->visible_right > 0 ? (size_t)t->visible_right : 0;
    size_t height = t->visible_top > 0 ? (size_t)t->visible_top : 0;
    size_t cell_count = width * height;
    row_cache_ensure(t, width, height);
    grid_grow_chars(t);
    if (cell_count > t->render_cells_len) {
        t->render_cells = realloc(t->render_cells, cell_count * sizeof(uint32_t));
        t->render_cells_len = cell_count;
    }
    render_log_bind(t);
    bool overflow = g_rlog.overflow;
    if (t->grid_incremental && t->grid_ready && t->lists_valid && !overflow) {
        // Few mutations: the change log keeps the grid current and the paint
        // walk is skipped entirely (the asm renderer's log replay).
        grid_replay(t, width);
    } else {
        bool lists = t->grid_incremental || t->want_lists;
        grid_walk(t, width, height, lists);
        t->lists_valid = lists;
        t->want_lists = false;
    }
    g_rlog.len = 0;
    g_rlog.overflow = false;
    // Mode selection. A frame that overflowed the log while replaying shows the
    // grid is changing faster than a log can carry it, so the walk takes over.
    // From the walk, one cheap probe frame (small cap, no list rebuild) decides
    // whether replaying would pay; heavy effects pay only that probe.
    if (t->grid_incremental) {
        if (overflow) {
            t->grid_incremental = false;
            t->lists_valid = false;
            t->walk_frames = 0;
        }
    } else if (t->probing) {
        t->probing = false;
        if (!overflow) {
            t->grid_incremental = true;
            t->want_lists = true;
        } else {
            t->walk_frames = 0;
        }
    } else if (++t->walk_frames >= 32) {
        t->probing = true;
    }
    g_rlog.on = t->probing || (t->grid_incremental && t->lists_valid);
    uint32_t limit = (uint32_t)(t->visible_characters_len / 4 + 64);
    g_rlog.limit = limit;
    while (g_rlog.cap < limit) {
        render_log_grow();
    }
    *out_width = width;
    *out_height = height;
}

// Copies a visual's bytes from its pool handle. The pool guarantees 128
// readable bytes from a visual's start, so short visuals use fixed-size
// vector copies rather than a libc call. Returns the visual's true length.
static inline size_t emit_handle(char *dst, VisualHandle h) {
    size_t len = visual_len(h);
    const char *src = visual_bytes(h);
    // Fixed-size copies inline to vector loads/stores. A variable-size memcpy
    // here otherwise calls libc once per painted cell, even for tiny glyphs.
    memcpy(dst, src, 16);
    if (len > 16) {
        memcpy(dst + 16, src + 16, 16);
        if (len > 32) {
            memcpy(dst + 32, src + 32, 16);
            if (len > 48) {
                memcpy(dst + 48, src + 48, 16);
                if (len > 64) {
                    memcpy(dst + 64, src + 64, (len - 64 + 15u) & ~15u);
                }
            }
        }
    }
    return len;
}

// Whole-row serialization, used by the render verifier.
static void serialize_row(const Terminal *t, size_t row_index, size_t width, StrBuf *dst) {
    const uint32_t *owners = &t->render_cells[row_index * width];
    const VisualHandle *handles = &t->cell_visual[row_index * width];
    static const char SPACES[] = "                                                                ";
    sb_reserve(dst, width * VISUAL_MAX + 2);
    size_t col = 0;
    while (col < width) {
        if (owners[col] == RENDER_EMPTY) {
            size_t run = 1;
            while (col + run < width && owners[col + run] == RENDER_EMPTY) {
                run++;
            }
            size_t remaining = run;
            while (remaining > 0) {
                size_t take = remaining > sizeof(SPACES) - 1 ? sizeof(SPACES) - 1 : remaining;
                sb_append(dst, SPACES, take);
                remaining -= take;
            }
            col += run;
        } else {
            dst->len += emit_handle(dst->data + dst->len, handles[col]);
            col++;
        }
    }
    dst->data[dst->len] = '\0';
}

// ---- row cache: one buffer pair per row, rebuilt by BLOCK-cell blocks ------
//
// A row keeps its bytes in one of two buffers and the byte offset at which
// every VISUAL_BLOCK-cell block starts. A frame compares the cell handles with
// the previous frame's; a row with no changed block is left alone, and a row
// with changed blocks is rebuilt into its other buffer by copying the runs of
// clean blocks (one piece each) and formatting only the runs of dirty ones
// (render.asm's row_rebuild). A clean gap shorter than VISUAL_MIN_GAP between
// dirty runs is formatted along with them, since a copy costs about as much as
// a few formatted cells.

#define VISUAL_BLOCK 4
#define VISUAL_MIN_GAP 4
// Rows with more dirty runs than this are rebuilt, not patched in place.
#define VISUAL_IN_PLACE_RUNS 1
// Row buffer room past the longest content: emit_handle's fixed-size copies
// overrun a cell by under 16 bytes, and in-place patching saves that much of
// the tail beyond the run it rewrites.
#define ROW_SLACK 64

static void row_cache_ensure(Terminal *t, size_t width, size_t height);

// Formats blocks [first, last) into `dst` from `len`, recording each block's
// start offset in offs[]; returns the new length.
static size_t format_blocks(const Terminal *t, size_t row, size_t width, size_t first, size_t last, char *dst,
                            size_t len, uint32_t *offs) {
    const VisualHandle *handles = &t->cell_visual[row * width];
    const uint32_t *owners = &t->render_cells[row * width];
    size_t end = last * VISUAL_BLOCK;
    if (end > width) end = width;
    for (size_t col = first * VISUAL_BLOCK; col < end; col++) {
        if (col % VISUAL_BLOCK == 0) {
            offs[col / VISUAL_BLOCK] = (uint32_t)len;
        }
        if (owners[col] == RENDER_EMPTY) {
            dst[len++] = ' ';
        } else {
            len += emit_handle(dst + len, handles[col]);
        }
    }
    return len;
}

// The bytes cells [first, end) format to.
static size_t cells_length(const Terminal *t, size_t row, size_t width, size_t first, size_t end) {
    const VisualHandle *handles = &t->cell_visual[row * width];
    const uint32_t *owners = &t->render_cells[row * width];
    size_t n = 0;
    for (size_t col = first; col < end; col++) {
        n += owners[col] == RENDER_EMPTY ? 1 : visual_len(handles[col]);
    }
    return n;
}

// Gather a row's dirty bytes into one bit per block and clear them; returns
// the number of dirty blocks. `dirty` is whole 64-cell chunks.
static size_t dirty_blocks(uint8_t *dirty, size_t chunks, uint64_t *bits) {
    size_t count = 0;
    size_t words = (chunks + 3) / 4;
    for (size_t w = 0; w < words; w++) {
        uint64_t word = 0;
        for (size_t k = 0; k < 4 && w * 4 + k < chunks; k++) {
            uint8_t *chunk = dirty + (w * 4 + k) * 64;
            uint32_t m = 0;
#if defined(__SSE2__)
            const __m128i zero = _mm_setzero_si128();
            uint32_t clean = 0;
            for (int i = 0; i < 4; i++) {
                __m128i v = _mm_loadu_si128((const __m128i *)(const void *)(chunk + 16 * i));
                // a block is clean when its four bytes (one u32 lane) are zero
                clean |= (uint32_t)_mm_movemask_ps(_mm_castsi128_ps(_mm_cmpeq_epi32(v, zero))) << (4 * i);
                _mm_storeu_si128((__m128i *)(void *)(chunk + 16 * i), zero);
            }
            m = ~clean & 0xffffu;
#else
            for (int i = 0; i < 16; i++) {
                uint32_t lane;
                memcpy(&lane, chunk + 4 * i, 4);
                m |= (uint32_t)(lane != 0) << i;
            }
            memset(chunk, 0, 64);
#endif
            word |= (uint64_t)m << (16 * k);
        }
        bits[w] = word;
        count += (size_t)__builtin_popcountll(word);
    }
    bits[words] = 0;
    return count;
}

// The first block >= b whose bit is set (a sentinel bit stops the scan).
static inline size_t next_set(const uint64_t *bits, size_t b) {
    size_t w = b >> 6;
    uint64_t m = bits[w] & (~UINT64_C(0) << (b & 63));
    while (m == 0) m = bits[++w];
    return w << 6 | (size_t)__builtin_ctzll(m);
}

// The first block >= b whose bit is clear, at most limit.
static inline size_t next_clear(const uint64_t *bits, size_t b, size_t limit) {
    size_t w = b >> 6;
    uint64_t m = ~bits[w] & (~UINT64_C(0) << (b & 63));
    while (m == 0) m = ~bits[++w];
    size_t at = w << 6 | (size_t)__builtin_ctzll(m);
    return at < limit ? at : limit;
}

// The dirty runs of blocks [b, e) (with the sentinel at `blocks`); a clean
// gap shorter than VISUAL_MIN_GAP joins the runs around it. Stops counting
// past `max` runs: the caller only distinguishes few from many.
static size_t dirty_runs(const uint64_t *bits, size_t blocks, size_t (*out)[2], size_t max) {
    size_t n = 0;
    size_t b = next_set(bits, 0);
    while (b < blocks) {
        size_t e = next_clear(bits, b, blocks);
        while (e < blocks) {
            size_t s = next_set(bits, e);
            if (s >= e + VISUAL_MIN_GAP || s >= blocks) break;
            e = next_clear(bits, s, blocks);
        }
        if (n == max) return n + 1;
        out[n][0] = b;
        out[n][1] = e;
        n++;
        if (e >= blocks) break;
        b = next_set(bits, e);
    }
    return n;
}

// Copy the clean blocks [from, to) of the previous buffer to `len` of the
// next, shifting their offsets; returns the new length.
static size_t copy_clean(const char *prev, const uint32_t *prev_offs, char *next, uint32_t *next_offs, size_t from,
                         size_t to, size_t len) {
    if (from >= to) return len;
    size_t start = prev_offs[from];
    size_t run = prev_offs[to] - start;
    for (size_t i = from; i < to; i++) {
        next_offs[i] = (uint32_t)(prev_offs[i] - start + len);
    }
    memcpy(next + len, prev + start, run);
    return len + run;
}

static void row_cache_ensure(Terminal *t, size_t width, size_t height) {
    if (t->cache_width == width && t->cache_height == height && t->row_store) {
        return;
    }
    for (size_t i = 0; i < t->row_store_len; i++) {
        free(t->row_store[i]);
    }
    free(t->row_store);
    free(t->row_bytes);
    free(t->row_lens);
    free(t->row_caps);
    free(t->row_sel);
    free(t->cell_visual);
    free(t->previous_cell_visual);
    free(t->row_dirty);
    free(t->cell_dirty);
    free(t->row_offs);
    free(t->block_bits);
    free(t->cell_head);
    free(t->cell_next);
    free(t->cell_of_char);
    free(t->render_mirror);
    t->render_mirror = NULL;
    t->render_mirror_len = 0;

    size_t cells = width * height;
    size_t blocks = (width + VISUAL_BLOCK - 1) / VISUAL_BLOCK;
    t->row_blocks = blocks;
    t->dirty_stride = (width + 63) / 64 * 64;
    size_t dirty_bytes = height * t->dirty_stride;
    t->cell_dirty = calloc(dirty_bytes ? dirty_bytes : 1, 1);
    t->row_offs = calloc(height ? height * 2 * (blocks + 1) : 1, sizeof(uint32_t));
    t->block_bits = calloc(blocks / 64 + 2, sizeof(uint64_t));
    if (!t->cell_dirty || !t->row_offs || !t->block_bits) {
        fputs("glyphfx: out of memory (row cache)\n", stderr);
        abort();
    }
    t->row_store_len = height * 2;
    t->row_store = calloc(t->row_store_len ? t->row_store_len : 1, sizeof(char *));
    t->row_bytes = calloc(height ? height : 1, sizeof(char *));
    t->row_lens = calloc(height ? height : 1, sizeof(size_t));
    t->row_caps = calloc(height ? height : 1, sizeof(size_t));
    t->row_sel = calloc(height ? height : 1, sizeof(uint8_t));
    t->cell_visual = calloc(cells ? cells : 1, sizeof(VisualHandle));
    t->previous_cell_visual = calloc(cells ? cells : 1, sizeof(VisualHandle));
    if (!t->cell_visual || !t->previous_cell_visual) {
        fputs("glyphfx: out of memory (visual grids)\n", stderr);
        abort();
    }
    t->row_dirty = calloc(height ? height : 1, sizeof(uint8_t));
    t->all_rows_dirty = true;
    t->cell_head = malloc((cells ? cells : 1) * sizeof(int32_t));
    t->cell_next = malloc((t->arena.len ? t->arena.len : 1) * sizeof(int32_t));
    t->cell_of_char = malloc((t->arena.len ? t->arena.len : 1) * sizeof(int32_t));
    t->render_mirror_len = t->arena.len;
    t->render_mirror = calloc(t->render_mirror_len ? t->render_mirror_len : 1, sizeof(RenderMirror));
    for (size_t id = 0; id < t->arena.len; id++) {
        mirror_fill(t, (CharId)id);
    }
    t->grid_chars_len = t->arena.len;
    t->grid_ready = false;
    t->lists_valid = false;
    t->want_lists = false;
    t->grid_incremental = false;
    t->walk_frames = 0;
    t->probing = false;
    t->cache_width = width;
    t->cache_height = height;
    t->cache_on = true;
    t->cache_probing = true;
    t->gate_seen = 0;
    t->gate_clean = 0;
    t->gate_painted = 0;
    t->probe_backoff = ROW_CACHE_PROBE;
    const char *verify = getenv("GLYPHFX_RENDER_VERIFY");
    g_render_verify = verify && verify[0] && verify[0] != '0';
}

static bool row_is_clean(const Terminal *t, size_t row_index) {
    return t->row_lens[row_index] != 0 && !t->all_rows_dirty && !t->row_dirty[row_index];
}

// Renders the frame into the per-row buffers.
void terminal_render_rows(Terminal *t) {
    size_t width = 0;
    size_t height = 0;
    update_render_cells(t, &width, &height);
    size_t clean_rows = 0;
    if (!t->row_store || t->cache_width != width || t->cache_height != height) {
        row_cache_ensure(t, width, height);
    }
    size_t blocks = t->row_blocks;
    size_t chunks = t->dirty_stride / 64;
    size_t ostride = blocks + 1;
    size_t runs[VISUAL_IN_PLACE_RUNS][2];
    for (size_t row = height; row-- > 0;) {
        if (row_is_clean(t, row)) {
            if (g_render_verify) {
                StrBuf check;
                sb_init(&check);
                serialize_row(t, row, width, &check);
                if (row != 0) {
                    sb_push(&check, '\n');
                }
                if (check.len != t->row_lens[row] ||
                    (check.len && memcmp(check.data, t->row_bytes[row], check.len) != 0)) {
                    fprintf(stderr, "render-verify: row %zu deemed clean but content changed\n", row);
                    abort();
                }
                sb_free(&check);
            }
            clean_rows++;
            continue;
        }
        size_t cap = width * VISUAL_MAX + 2 + ROW_SLACK;  // every cell at its longest, newline, slack
        if (t->row_caps[row] < cap) {
            t->row_store[row * 2] = realloc(t->row_store[row * 2], cap);
            t->row_store[row * 2 + 1] = realloc(t->row_store[row * 2 + 1], cap);
            if (!t->row_store[row * 2] || !t->row_store[row * 2 + 1]) {
                fputs("glyphfx: out of memory (row buffers)\n", stderr);
                abort();
            }
            t->row_caps[row] = cap;
            t->row_bytes[row] = t->row_store[row * 2 + t->row_sel[row]];
        }
        uint8_t *dirty = t->cell_dirty + row * t->dirty_stride;
        t->row_dirty[row] = 0;
        // fx's emission: a row never built, or mostly dirty, is formatted
        // whole; a single dirty run of blocks is patched in place; more runs
        // rebuild the row into its other buffer, copying the clean blocks.
        bool full = t->all_rows_dirty || t->row_lens[row] == 0;
        size_t n_runs = 0;
        if (full) {
            memset(dirty, 0, t->dirty_stride);
        } else {
            size_t count = dirty_blocks(dirty, chunks, t->block_bits);
            if (count == 0) {
                clean_rows++;
                continue;
            }
            full = count * 4 >= blocks * 3;
            t->block_bits[blocks >> 6] |= UINT64_C(1) << (blocks & 63);  // sentinel
            if (!full) {
                n_runs = dirty_runs(t->block_bits, blocks, runs, VISUAL_IN_PLACE_RUNS);
            }
        }
        uint8_t sel = t->row_sel[row];
        char *cur = t->row_store[row * 2 + sel];
        uint32_t *cur_offs = t->row_offs + (row * 2 + sel) * ostride;
        if (!full && n_runs <= VISUAL_IN_PLACE_RUNS) {
            // Patch in place, right to left so the offsets of runs still
            // ahead stay valid. The tail moves only when a run's length
            // changed; emit_handle overruns its cell by under 16 bytes, so
            // the tail's first bytes are saved and put back.
            size_t len = t->row_lens[row];
            for (size_t r = n_runs; r-- > 0;) {
                size_t b = runs[r][0], e = runs[r][1];
                size_t end = e * VISUAL_BLOCK < width ? e * VISUAL_BLOCK : width;
                size_t n = cells_length(t, row, width, b * VISUAL_BLOCK, end);
                size_t start = cur_offs[b], old_end = cur_offs[e];
                size_t old = old_end - start;
                if (n != old) {
                    memmove(cur + start + n, cur + old_end, len - old_end);
                    len = len + n - old;
                    for (size_t i = e; i <= blocks; i++) {
                        cur_offs[i] = (uint32_t)(cur_offs[i] + n - old);
                    }
                }
                char saved[16];
                memcpy(saved, cur + start + n, sizeof(saved));
                (void)format_blocks(t, row, width, b, e, cur, start, cur_offs);
                memcpy(cur + start + n, saved, sizeof(saved));
            }
            cur[len] = '\0';
            t->row_lens[row] = len;
            continue;
        }
        char *out = t->row_store[row * 2 + (sel ^ 1)];
        uint32_t *out_offs = t->row_offs + (row * 2 + (sel ^ 1)) * ostride;
        size_t len = 0;
        if (full) {
            len = format_blocks(t, row, width, 0, blocks, out, 0, out_offs);
        } else {
            // Rebuild: copy each clean stretch, format each dirty run.
            size_t pos = 0;
            size_t b = next_set(t->block_bits, 0);
            while (b < blocks) {
                size_t e = next_clear(t->block_bits, b, blocks);
                while (e < blocks) {
                    size_t s = next_set(t->block_bits, e);
                    if (s >= e + VISUAL_MIN_GAP || s >= blocks) break;
                    e = next_clear(t->block_bits, s, blocks);
                }
                len = copy_clean(cur, cur_offs, out, out_offs, pos, b, len);
                len = format_blocks(t, row, width, b, e, out, len, out_offs);
                pos = e;
                if (e >= blocks) break;
                b = next_set(t->block_bits, e);
            }
            len = copy_clean(cur, cur_offs, out, out_offs, pos, blocks, len);
        }
        out_offs[blocks] = (uint32_t)len;
        if (row != 0) {
            out[len++] = '\n';
        }
        out[len] = '\0';
        t->row_sel[row] = sel ^ 1;
        t->row_bytes[row] = out;
        t->row_lens[row] = len;
    }
    if (g_render_verify) {
        // Every row, patched, rebuilt or kept, must equal a whole-row format.
        for (size_t row = 0; row < height; row++) {
            StrBuf check;
            sb_init(&check);
            serialize_row(t, row, width, &check);
            if (row != 0) {
                sb_push(&check, '\n');
            }
            if (check.len != t->row_lens[row] ||
                (check.len && memcmp(check.data, t->row_bytes[row], check.len) != 0)) {
                fprintf(stderr, "render-verify: row %zu emitted wrong bytes\n", row);
                abort();
            }
            sb_free(&check);
        }
    }
    t->all_rows_dirty = false;
    t->last_width = width;
    t->last_height = height;
    t->last_clean_rows = clean_rows;
}

static void frame_gate(Terminal *t) {
    t->gate_painted = 0;
    t->gate_seen = 0;
    t->gate_clean = 0;
}

const char *terminal_get_formatted_output_string(Terminal *t) {
    terminal_render_rows(t);
    size_t width = t->visible_right > 0 ? (size_t)t->visible_right : 0;
    size_t height = t->visible_top > 0 ? (size_t)t->visible_top : 0;
    (void)width;
    StrBuf *sb = &t->output_buffer;
    sb_clear(sb);
    for (size_t row_index = height; row_index-- > 0;) {
        sb_append(sb, t->row_bytes[row_index], t->row_lens[row_index]);
    }
    frame_gate(t);
    return sb->data ? sb->data : "";
}

// Emit the rendered rows with one writev (cursor prefix plus one iovec per
// row), so the frame is never assembled into a single string. rows already
// carry their joining newline.
int terminal_emit_frame(Terminal *t, FILE *out) {
    size_t height = t->visible_top > 0 ? (size_t)t->visible_top : 0;
    if (height == 0) {
        return 0;
    }
    size_t need = height + 1;
    if (t->frame_iov_cap < need) {
        struct iovec *grown = realloc(t->frame_iov, need * sizeof(struct iovec));
        if (!grown) {
            return -1;
        }
        t->frame_iov = grown;
        t->frame_iov_cap = need;
    }
    if (fflush(out) != 0) {
        return -1;
    }
    const char *prefix = t->move_cursor_to_top ? t->move_cursor_to_top : "";
    t->frame_iov[0].iov_base = (void *)prefix;
    t->frame_iov[0].iov_len = strlen(prefix);
    size_t n = 1;
    for (size_t row = height; row-- > 0;) {
        t->frame_iov[n].iov_base = t->row_bytes[row];
        t->frame_iov[n].iov_len = t->row_lens[row];
        n++;
    }
    int fd = fileno(out);
    size_t max_iov = 1024;
#ifdef IOV_MAX
    max_iov = IOV_MAX;
#endif
    if (max_iov < 1) {
        max_iov = 1;
    }
    size_t i = 0;
    while (i < n) {
        size_t chunk = n - i;
        if (chunk > max_iov) {
            chunk = max_iov;
        }
        ssize_t written;
        do {
            written = writev(fd, &t->frame_iov[i], (int)chunk);
        } while (written < 0 && errno == EINTR);
        if (written < 0) {
            return -1;
        }
        size_t consumed = (size_t)written;
        while (i < n && consumed >= t->frame_iov[i].iov_len) {
            consumed -= t->frame_iov[i].iov_len;
            i++;
        }
        if (i < n && consumed > 0) {
            t->frame_iov[i].iov_base = (char *)t->frame_iov[i].iov_base + consumed;
            t->frame_iov[i].iov_len -= consumed;
        }
    }
    frame_gate(t);
    return 0;
}

// --- tty side -------------------------------------------------------------

void terminal_prep_canvas(Terminal *t, FILE *out) {
    fputs(ANSI_HIDE_CURSOR, out);
    if (t->config.reuse_canvas) {
        fputs(t->move_cursor_to_top, out);
    }
    int64_t blank = t->visible_right > 0 ? t->visible_right : 0;
    for (int64_t i = 0; i < t->visible_top; i++) {
        for (int64_t j = 0; j < blank; j++) {
            fputc(' ', out);
        }
        fputc('\n', out);
    }
    fputs(ANSI_DEC_SAVE_CURSOR, out);
}

void terminal_restore_cursor(Terminal *t, FILE *out, const char *end_symbol) {
    const char *symbol = t->config.no_eol ? "" : end_symbol;
    if (!t->config.no_restore_cursor) {
        fputs(ANSI_SHOW_CURSOR, out);
    }
    fputs(symbol, out);
}

void terminal_print_frame(Terminal *t, FILE *out, const char *output_string) {
    fputs(t->move_cursor_to_top, out);
    fputs(output_string, out);
    fflush(out);
}

void terminal_reset_canvas_area(Terminal *t, FILE *out) {
    fputs(ANSI_DEC_RESTORE_CURSOR, out);
    if (t->visible_top > 0) {
        StrBuf sb;
        sb_init(&sb);
        ansi_move_cursor_up(&sb, t->visible_top);
        fputs(sb.data, out);
        sb_free(&sb);
    }
    fputs(ANSI_CLEAR_TO_END_OF_SCREEN, out);
}

void terminal_enforce_framerate(Terminal *t) {
    if (t->frame_rate == 0) {
        return;
    }
    double frame_delay = 1.0 / (double)t->frame_rate;
    double elapsed = monotonic_seconds() - t->last_time_printed;
    if (elapsed < frame_delay) {
        double remaining = frame_delay - elapsed;
        struct timespec ts;
        ts.tv_sec = (time_t)remaining;
        ts.tv_nsec = (long)((remaining - (double)ts.tv_sec) * 1e9);
        nanosleep(&ts, NULL);
    }
    t->last_time_printed = monotonic_seconds();
}

// --- character queries ----------------------------------------------------

CharacterFilter character_filter_default(void) {
    CharacterFilter filter;
    filter.input_chars = true;
    filter.inner_fill_chars = false;
    filter.outer_fill_chars = false;
    filter.added_chars = false;
    return filter;
}

static int sort_key_compare(const Arena *arena, CharId a, CharId b, int mode) {
    Coord ca = arena->items[a].input_coord;
    Coord cb = arena->items[b].input_coord;
    if (mode == 0) {
        // (-row, column)
        if (ca.row != cb.row) return ca.row > cb.row ? -1 : 1;
        if (ca.column != cb.column) return ca.column < cb.column ? -1 : 1;
    } else {
        // (row, column)
        if (ca.row != cb.row) return ca.row < cb.row ? -1 : 1;
        if (ca.column != cb.column) return ca.column < cb.column ? -1 : 1;
    }
    return 0;
}

// Stable merge sort over the (mode) key. Ties keep their input order, matching
// the reference's stable sort_by_key.
static void stable_sort_ids(CharId *ids, size_t n, const Arena *arena, int mode) {
    if (n < 2) {
        return;
    }
    CharId *tmp = malloc(n * sizeof(CharId));
    if (!tmp) {
        return;
    }
    for (size_t width = 1; width < n; width *= 2) {
        for (size_t lo = 0; lo < n; lo += 2 * width) {
            size_t mid = lo + width < n ? lo + width : n;
            size_t hi = lo + 2 * width < n ? lo + 2 * width : n;
            size_t i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                if (sort_key_compare(arena, ids[j], ids[i], mode) < 0) {
                    tmp[k++] = ids[j++];
                } else {
                    tmp[k++] = ids[i++];
                }
            }
            while (i < mid) tmp[k++] = ids[i++];
            while (j < hi) tmp[k++] = ids[j++];
        }
        memcpy(ids, tmp, n * sizeof(CharId));
    }
    free(tmp);
}

CharId *terminal_get_characters(const Terminal *t, Rng *rng, CharacterFilter filter, CharacterSort sort,
                                size_t *out_len) {
    size_t cap = 0;
    if (filter.input_chars) cap += t->input_characters_len;
    if (filter.inner_fill_chars) cap += t->inner_fill_characters_len;
    if (filter.outer_fill_chars) cap += t->outer_fill_characters_len;
    if (filter.added_chars) cap += t->added_characters_len;
    CharId *all = malloc((cap ? cap : 1) * sizeof(CharId));
    size_t n = 0;
    if (filter.input_chars && t->input_characters_len) {
        memcpy(all + n, t->input_characters, t->input_characters_len * sizeof(CharId));
        n += t->input_characters_len;
    }
    if (filter.inner_fill_chars && t->inner_fill_characters_len) {
        memcpy(all + n, t->inner_fill_characters, t->inner_fill_characters_len * sizeof(CharId));
        n += t->inner_fill_characters_len;
    }
    if (filter.outer_fill_chars && t->outer_fill_characters_len) {
        memcpy(all + n, t->outer_fill_characters, t->outer_fill_characters_len * sizeof(CharId));
        n += t->outer_fill_characters_len;
    }
    if (filter.added_chars && t->added_characters_len) {
        memcpy(all + n, t->added_characters, t->added_characters_len * sizeof(CharId));
        n += t->added_characters_len;
    }
    stable_sort_ids(all, n, &t->arena, 0);
    switch (sort) {
        case CS_RANDOM:
            rng_shuffle(rng, all, n, sizeof(CharId));
            break;
        case CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT:
            break;
        case CS_BOTTOM_TO_TOP_RIGHT_TO_LEFT:
            for (size_t i = 0; i < n / 2; i++) {
                CharId tmp = all[i];
                all[i] = all[n - 1 - i];
                all[n - 1 - i] = tmp;
            }
            break;
        case CS_BOTTOM_TO_TOP_LEFT_TO_RIGHT:
        case CS_TOP_TO_BOTTOM_RIGHT_TO_LEFT:
            stable_sort_ids(all, n, &t->arena, 1);
            if (sort == CS_TOP_TO_BOTTOM_RIGHT_TO_LEFT) {
                for (size_t i = 0; i < n / 2; i++) {
                    CharId tmp = all[i];
                    all[i] = all[n - 1 - i];
                    all[n - 1 - i] = tmp;
                }
            }
            break;
        case CS_OUTSIDE_ROW_TO_MIDDLE:
        case CS_MIDDLE_ROW_TO_OUTSIDE: {
            CharId *interleaved = malloc((n ? n : 1) * sizeof(CharId));
            size_t lo = 0, hi = n, k = 0;
            bool from_front = true;
            while (lo < hi) {
                interleaved[k++] = from_front ? all[lo++] : all[--hi];
                from_front = !from_front;
            }
            memcpy(all, interleaved, n * sizeof(CharId));
            free(interleaved);
            if (sort == CS_MIDDLE_ROW_TO_OUTSIDE) {
                for (size_t i = 0; i < n / 2; i++) {
                    CharId tmp = all[i];
                    all[i] = all[n - 1 - i];
                    all[n - 1 - i] = tmp;
                }
            }
            break;
        }
    }
    *out_len = n;
    return all;
}

// --- grouped queries ------------------------------------------------------

void charidgrouping_free(CharIdGrouping *g) {
    for (size_t i = 0; i < g->len; i++) {
        free(g->buckets[i].items);
    }
    free(g->buckets);
    g->buckets = NULL;
    g->len = 0;
}

static CharIdGrouping ordered_buckets(const CharId *chars, size_t n, int64_t first_key, int64_t last_key,
                                      const int64_t *keys) {
    CharIdGrouping out;
    out.buckets = NULL;
    out.len = 0;
    if (first_key > last_key) {
        return out;
    }
    int64_t span = last_key - first_key + 1;
    size_t bucket_count = (size_t)span;
    CharIdBucket *buckets = calloc(bucket_count, sizeof(CharIdBucket));
    for (size_t i = 0; i < n; i++) {
        int64_t key = keys[i];
        if (key >= first_key && key <= last_key) {
            size_t b = (size_t)(key - first_key);
            CharIdBucket *bucket = &buckets[b];
            bucket->items = realloc(bucket->items, (bucket->len + 1) * sizeof(CharId));
            bucket->items[bucket->len++] = chars[i];
        }
    }
    for (size_t i = 0; i < bucket_count; i++) {
        if (buckets[i].len > 0) {
            if (out.len == 0) {
                out.buckets = malloc(sizeof(CharIdBucket));
            } else {
                out.buckets = realloc(out.buckets, (out.len + 1) * sizeof(CharIdBucket));
            }
            out.buckets[out.len++] = buckets[i];
        } else {
            free(buckets[i].items);
        }
    }
    free(buckets);
    return out;
}

CharIdGrouping terminal_get_characters_grouped(const Terminal *t, CharacterFilter filter, CharacterGroup grouping) {
    size_t n = 0;
    CharId *all = terminal_get_characters(t, NULL, filter, CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT, &n);
    // reference re-sorts by (row, column) here, replacing the default (-row,col)
    stable_sort_ids(all, n, &t->arena, 1);
    int64_t *keys = malloc((n ? n : 1) * sizeof(int64_t));
    CharIdGrouping out;
    out.buckets = NULL;
    out.len = 0;
    switch (grouping) {
        case CG_COLUMN_LEFT_TO_RIGHT:
        case CG_COLUMN_RIGHT_TO_LEFT:
            for (size_t i = 0; i < n; i++) keys[i] = t->arena.items[all[i]].input_coord.column;
            out = ordered_buckets(all, n, 0, t->canvas.right, keys);
            if (grouping == CG_COLUMN_RIGHT_TO_LEFT) {
                for (size_t i = 0; i < out.len / 2; i++) {
                    CharIdBucket tmp = out.buckets[i];
                    out.buckets[i] = out.buckets[out.len - 1 - i];
                    out.buckets[out.len - 1 - i] = tmp;
                }
            }
            break;
        case CG_ROW_BOTTOM_TO_TOP:
        case CG_ROW_TOP_TO_BOTTOM:
            for (size_t i = 0; i < n; i++) keys[i] = t->arena.items[all[i]].input_coord.row;
            out = ordered_buckets(all, n, 0, t->canvas.top, keys);
            if (grouping == CG_ROW_TOP_TO_BOTTOM) {
                for (size_t i = 0; i < out.len / 2; i++) {
                    CharIdBucket tmp = out.buckets[i];
                    out.buckets[i] = out.buckets[out.len - 1 - i];
                    out.buckets[out.len - 1 - i] = tmp;
                }
            }
            break;
        case CG_DIAGONAL_BOTTOM_LEFT_TO_TOP_RIGHT:
        case CG_DIAGONAL_TOP_RIGHT_TO_BOTTOM_LEFT:
            for (size_t i = 0; i < n; i++) {
                Coord c = t->arena.items[all[i]].input_coord;
                keys[i] = c.row + c.column;
            }
            out = ordered_buckets(all, n, 0, t->canvas.top + t->canvas.right, keys);
            if (grouping == CG_DIAGONAL_TOP_RIGHT_TO_BOTTOM_LEFT) {
                for (size_t i = 0; i < out.len / 2; i++) {
                    CharIdBucket tmp = out.buckets[i];
                    out.buckets[i] = out.buckets[out.len - 1 - i];
                    out.buckets[out.len - 1 - i] = tmp;
                }
            }
            break;
        case CG_DIAGONAL_TOP_LEFT_TO_BOTTOM_RIGHT:
        case CG_DIAGONAL_BOTTOM_RIGHT_TO_TOP_LEFT:
            for (size_t i = 0; i < n; i++) {
                Coord c = t->arena.items[all[i]].input_coord;
                keys[i] = c.column - c.row;
            }
            out = ordered_buckets(all, n, t->canvas.left - t->canvas.top, t->canvas.right - t->canvas.bottom, keys);
            if (grouping == CG_DIAGONAL_BOTTOM_RIGHT_TO_TOP_LEFT) {
                for (size_t i = 0; i < out.len / 2; i++) {
                    CharIdBucket tmp = out.buckets[i];
                    out.buckets[i] = out.buckets[out.len - 1 - i];
                    out.buckets[out.len - 1 - i] = tmp;
                }
            }
            break;
        case CG_CENTER_TO_OUTSIDE:
        case CG_OUTSIDE_TO_CENTER: {
            int64_t max_distance = -1;
            for (size_t i = 0; i < n; i++) {
                Coord c = t->arena.items[all[i]].input_coord;
                int64_t d = llabs(c.column - t->canvas.text_center.column) +
                            llabs(c.row - t->canvas.text_center.row);
                keys[i] = d;
                if (d > max_distance) max_distance = d;
            }
            size_t dense_limit = n * 4 > 256 ? n * 4 : 256;
            if (max_distance >= 0 && (size_t)max_distance <= dense_limit) {
                out = ordered_buckets(all, n, 0, max_distance, keys);
            } else {
                // sparse: bucket by distinct distance, ascending
                for (size_t i = 1; i < n; i++) {
                    int64_t k = keys[i];
                    CharId id = all[i];
                    size_t j = i;
                    while (j > 0 && keys[j - 1] > k) {
                        keys[j] = keys[j - 1];
                        all[j] = all[j - 1];
                        j--;
                    }
                    keys[j] = k;
                    all[j] = id;
                }
                size_t cap = 0;
                for (size_t i = 0; i < n;) {
                    size_t j = i;
                    while (j < n && keys[j] == keys[i]) j++;
                    if (out.len == cap) {
                        cap = cap ? cap * 2 : 8;
                        out.buckets = realloc(out.buckets, cap * sizeof(CharIdBucket));
                    }
                    CharIdBucket bucket;
                    bucket.len = j - i;
                    bucket.items = malloc(bucket.len * sizeof(CharId));
                    memcpy(bucket.items, all + i, bucket.len * sizeof(CharId));
                    out.buckets[out.len++] = bucket;
                    i = j;
                }
            }
            if (grouping == CG_OUTSIDE_TO_CENTER) {
                for (size_t i = 0; i < out.len / 2; i++) {
                    CharIdBucket tmp = out.buckets[i];
                    out.buckets[i] = out.buckets[out.len - 1 - i];
                    out.buckets[out.len - 1 - i] = tmp;
                }
            }
            break;
        }
    }
    free(keys);
    free(all);
    return out;
}

Color *terminal_get_input_colors(const Terminal *t, Rng *rng, ColorSort sort, size_t *out_len) {
    size_t n = t->input_colors_frequency.len;
    ColorFreqEntry *entries = malloc((n ? n : 1) * sizeof(ColorFreqEntry));
    memcpy(entries, t->input_colors_frequency.items, n * sizeof(ColorFreqEntry));
    if (sort == COLOR_SORT_RANDOM) {
        rng_shuffle(rng, entries, n, sizeof(ColorFreqEntry));
    } else {
        bool desc = sort == COLOR_SORT_MOST_TO_LEAST;
        // stable insertion sort by count, descending or ascending
        for (size_t i = 1; i < n; i++) {
            ColorFreqEntry e = entries[i];
            size_t j = i;
            while (j > 0 && (desc ? entries[j - 1].count < e.count : entries[j - 1].count > e.count)) {
                entries[j] = entries[j - 1];
                j--;
            }
            entries[j] = e;
        }
    }
    Color *out = malloc((n ? n : 1) * sizeof(Color));
    for (size_t i = 0; i < n; i++) {
        out[i] = entries[i].color;
    }
    free(entries);
    *out_len = n;
    return out;
}

bool terminal_resize_settled(Terminal *t) {
    const double QUIET = 0.05;
    if (take_terminal_resize()) {
        t->resize_seen = true;
        t->resize_seen_time = monotonic_seconds();
    }
    if (!t->resize_seen) {
        return false;
    }
    if (monotonic_seconds() - t->resize_seen_time >= QUIET) {
        t->resize_seen = false;
    } else {
        return false;
    }
    if (t->config.ignore_terminal_dimensions) {
        return false;
    }
    int64_t width = 0;
    int64_t height = 0;
    terminal_get_dimensions(&width, &height);
    if (width == t->terminal_width && height == t->terminal_height) {
        return false;
    }
    Layout next = compute_layout(&t->config, t->input_line_lengths, t->input_line_lengths_len, width, height);
    return next.canvas_height != t->layout.canvas_height || next.canvas_width != t->layout.canvas_width ||
           next.column_offset != t->layout.column_offset || next.row_offset != t->layout.row_offset ||
           next.visible_top != t->layout.visible_top || next.visible_bottom != t->layout.visible_bottom ||
           next.visible_right != t->layout.visible_right || next.visible_left != t->layout.visible_left;
}
