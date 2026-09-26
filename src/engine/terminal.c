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

void terminal_add_character(Terminal *t, const char *symbol, Coord coord) {
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
    t->visible_positions_len = t->arena.len;
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
        int64_t row = ch->motion_coord.row + t->canvas_row_offset;
        int64_t column = ch->motion_coord.column + t->canvas_column_offset;
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
