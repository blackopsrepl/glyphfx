#include "engine/terminal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/ioctl.h>
#endif

#include "utils/pycompat.h"

#define EMPTY_RENDER_CELL UINT32_MAX
#define NOT_VISIBLE SIZE_MAX

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
    free(t->move_cursor_to_top);
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

static void update_render_cells(Terminal *t, size_t *out_width, size_t *out_height) {
    size_t width = t->visible_right > 0 ? (size_t)t->visible_right : 0;
    size_t height = t->visible_top > 0 ? (size_t)t->visible_top : 0;
    size_t cell_count = width * height;
    if (cell_count > t->render_cells_len) {
        t->render_cells = realloc(t->render_cells, cell_count * sizeof(uint32_t));
        t->render_cells_len = cell_count;
    }
    for (size_t i = 0; i < cell_count; i++) {
        t->render_cells[i] = EMPTY_RENDER_CELL;
    }
    for (size_t i = 0; i < t->visible_characters_len; i++) {
        CharId id = t->visible_characters[i];
        EffectCharacter *ch = &t->arena.items[id];
        int64_t row = ch->motion.current_coord.row + t->canvas_row_offset;
        int64_t column = ch->motion.current_coord.column + t->canvas_column_offset;
        if (t->visible_bottom <= row && row <= t->visible_top && t->visible_left <= column &&
            column <= t->visible_right) {
            size_t cell_index = (size_t)(row - 1) * width + (size_t)(column - 1);
            uint32_t cell = t->render_cells[cell_index];
            if (cell == EMPTY_RENDER_CELL) {
                t->render_cells[cell_index] = (uint32_t)id;
            } else {
                EffectCharacter *painted = &t->arena.items[cell];
                if (ch->layer > painted->layer ||
                    (ch->layer == painted->layer && ch->character_id > painted->character_id)) {
                    t->render_cells[cell_index] = (uint32_t)id;
                }
            }
        }
    }
    *out_width = width;
    *out_height = height;
}

char *terminal_get_formatted_output_string(Terminal *t) {
    size_t width = 0;
    size_t height = 0;
    update_render_cells(t, &width, &height);
    StrBuf sb;
    sb_init(&sb);
    for (size_t row_index = height; row_index-- > 0;) {
        if (row_index + 1 < height) {
            sb_push(&sb, '\n');
        }
        for (size_t col = 0; col < width; col++) {
            uint32_t cell = t->render_cells[row_index * width + col];
            if (cell == EMPTY_RENDER_CELL) {
                sb_push(&sb, ' ');
            } else {
                sb_puts(&sb, t->arena.items[cell].animation.current_visual.formatted);
            }
        }
    }
    return sb_take(&sb);
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
    if (filter.input_chars) {
        memcpy(all + n, t->input_characters, t->input_characters_len * sizeof(CharId));
        n += t->input_characters_len;
    }
    if (filter.inner_fill_chars) {
        memcpy(all + n, t->inner_fill_characters, t->inner_fill_characters_len * sizeof(CharId));
        n += t->inner_fill_characters_len;
    }
    if (filter.outer_fill_chars) {
        memcpy(all + n, t->outer_fill_characters, t->outer_fill_characters_len * sizeof(CharId));
        n += t->outer_fill_characters_len;
    }
    if (filter.added_chars) {
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
