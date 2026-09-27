// Terminal: config, canvas assembly, character queries, renderer, tty writer.
// Ported from the reference engine/terminal.py. A single Terminal owns both the
// simulation and the tty side.
#ifndef GLYPHFX_TERMINAL_H
#define GLYPHFX_TERMINAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "engine/animation.h"
#include "engine/canvas.h"
#include "engine/character.h"
#include "engine/input.h"
#include "utils/clock.h"
#include "utils/graphics.h"

typedef struct Terminal Terminal;

typedef struct TerminalConfig {
    int64_t tab_width;
    bool xterm_colors;
    bool no_color;
    Color terminal_background_color;
    ExistingColorHandling existing_color_handling;
    bool wrap_text;
    int64_t frame_rate;
    int64_t canvas_width;
    int64_t canvas_height;
    Anchor anchor_canvas;
    Anchor anchor_text;
    bool ignore_terminal_dimensions;
    bool reuse_canvas;
    bool no_eol;
    bool no_restore_cursor;
} TerminalConfig;

void terminal_config_default(TerminalConfig *config);

typedef enum {
    CS_RANDOM,
    CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT,
    CS_BOTTOM_TO_TOP_RIGHT_TO_LEFT,
    CS_BOTTOM_TO_TOP_LEFT_TO_RIGHT,
    CS_TOP_TO_BOTTOM_RIGHT_TO_LEFT,
    CS_OUTSIDE_ROW_TO_MIDDLE,
    CS_MIDDLE_ROW_TO_OUTSIDE,
} CharacterSort;

typedef struct {
    bool input_chars;
    bool inner_fill_chars;
    bool outer_fill_chars;
    bool added_chars;
} CharacterFilter;

typedef enum {
    CG_COLUMN_LEFT_TO_RIGHT,
    CG_COLUMN_RIGHT_TO_LEFT,
    CG_ROW_TOP_TO_BOTTOM,
    CG_ROW_BOTTOM_TO_TOP,
    CG_DIAGONAL_BOTTOM_LEFT_TO_TOP_RIGHT,
    CG_DIAGONAL_TOP_RIGHT_TO_BOTTOM_LEFT,
    CG_DIAGONAL_TOP_LEFT_TO_BOTTOM_RIGHT,
    CG_DIAGONAL_BOTTOM_RIGHT_TO_TOP_LEFT,
    CG_CENTER_TO_OUTSIDE,
    CG_OUTSIDE_TO_CENTER,
} CharacterGroup;

typedef enum {
    COLOR_SORT_LEAST_TO_MOST,
    COLOR_SORT_MOST_TO_LEAST,
    COLOR_SORT_RANDOM,
} ColorSort;

typedef struct {
    CharId *items;
    size_t len;
} CharIdBucket;

typedef struct {
    CharIdBucket *buckets;
    size_t len;
} CharIdGrouping;

void charidgrouping_free(CharIdGrouping *g);

CharacterFilter character_filter_default(void);
// Returns a malloc'd, ordered character list (caller frees).
CharId *terminal_get_characters(const Terminal *t, Rng *rng, CharacterFilter filter, CharacterSort sort,
                                size_t *out_len);
CharIdGrouping terminal_get_characters_grouped(const Terminal *t, CharacterFilter filter, CharacterGroup grouping);
// Returns a malloc'd color list (caller frees).
Color *terminal_get_input_colors(const Terminal *t, Rng *rng, ColorSort sort, size_t *out_len);

typedef struct {
    int64_t canvas_height;
    int64_t canvas_width;
    int64_t column_offset;
    int64_t row_offset;
    int64_t visible_top;
    int64_t visible_bottom;
    int64_t visible_right;
    int64_t visible_left;
} Layout;

struct Terminal {
    TerminalConfig config;
    Canvas canvas;
    Arena arena;
    uint32_t next_character_id;
    ColorFrequency input_colors_frequency;
    int64_t terminal_width;
    int64_t terminal_height;
    Layout layout;
    int64_t *input_line_lengths;
    size_t input_line_lengths_len;
    int64_t canvas_column_offset;
    int64_t canvas_row_offset;
    int64_t visible_top;
    int64_t visible_bottom;
    int64_t visible_right;
    int64_t visible_left;
    CharId *input_characters;
    size_t input_characters_len;
    CharId *added_characters;
    size_t added_characters_len;
    // Dense coordinate map over [1..map_right] x [1..map_top]; CHAR_ID_NONE
    // when empty. The reference uses an unordered map, but its keys are always
    // in-canvas coordinates, so a dense table is behaviorally identical.
    CharId *coord_map;
    int64_t map_right;
    int64_t map_top;
    CharId *inner_fill_characters;
    size_t inner_fill_characters_len;
    CharId *outer_fill_characters;
    size_t outer_fill_characters_len;
    CharId *visible_characters;
    size_t visible_characters_len;
    size_t *visible_positions;
    size_t visible_positions_len;
    uint32_t *render_cells;
    size_t render_cells_len;
    // Incremental grid state (the asm renderer's owner grid, occupant lists
    // and per-character mirrors). When mutations are few, the change log
    // maintains this grid and the paint walk is skipped entirely.
    int32_t *cell_head;      // per cell: first occupant, or -1
    int32_t *cell_next;      // per character: next occupant in its cell
    int32_t *cell_of_char;   // per character: its cell, or -1
    int64_t *cell_layer;     // per character: layer at the last replay
    size_t grid_chars_len;
    bool grid_ready;
    bool lists_valid;
    bool want_lists;
    bool grid_incremental;
    size_t walk_frames;
    bool probing;
    // Frame cache: a row whose cell owners and visual versions all match the
    // previous emitted frame is copied from its cached bytes, not re-formatted
    // (the asm renderer's row buffers). The version grid identifies rendered
    // byte content exactly, so no mutation hooks are needed.
    VisualHandle *cell_visual;  // winning visual per cell (a pool handle; 0 = empty)
    // Rows whose bytes must be rebuilt this frame. A cell write that changes
    // the winning handle marks its row, so the renderer finds dirty rows
    // without copying or rescanning the grid (render.asm's dirty cells).
    uint8_t *row_dirty;
    bool all_rows_dirty;
    uint8_t *row_sel;         // which of a row's two buffers is current
    size_t row_blocks;
    char **row_bytes;         // each row's current buffer
    size_t *row_lens;
    size_t *row_caps;
    char **row_store;         // height*2 buffers: the pair each row ping-pongs
    size_t row_store_len;
    size_t cache_width;
    size_t cache_height;
    bool cache_on;
    bool cache_probing;
    size_t painted_cells;     // winning paints this frame
    size_t gate_painted;      // painted cells over the gate window
    size_t gate_seen;
    size_t gate_clean;
    size_t probe_backoff;
    bool frame_rows_mode;   // render into row buffers and emit with writev
    size_t last_width;
    size_t last_height;
    size_t last_clean_rows;
    StrBuf output_buffer;  // reused across frames
    struct iovec *frame_iov;
    size_t frame_iov_cap;
    char *move_cursor_to_top;
    int64_t frame_rate;
    double last_time_printed;
    Clock clock;
    bool resize_seen;
    double resize_seen_time;
};

// Builds the terminal (preprocess + canvas + fill + neighbors). Returns 0 on
// success, or a PreprocessError via *err. On success *err->status == OK.
int terminal_new(Terminal *t, const char *input_data, const TerminalConfig *config, PreprocessError *err);
void terminal_free(Terminal *t);

CharId terminal_get_character_by_input_coord(const Terminal *t, Coord coord);
CharId terminal_add_character(Terminal *t, const char *symbol, Coord coord);
void terminal_set_character_visibility(Terminal *t, CharId id, bool is_visible);

// Refreshes the cell buffer and returns the frame string (rows top-first,
// '\n'-joined). Caller frees.
const char *terminal_get_formatted_output_string(Terminal *t);

// --- tty side ---
void terminal_prep_canvas(Terminal *t, FILE *out);
void terminal_restore_cursor(Terminal *t, FILE *out, const char *end_symbol);
void terminal_print_frame(Terminal *t, FILE *out, const char *output_string);
// Render the frame into the per-row buffers (frame_rows_mode) and emit the
// rows with one writev, so the frame is never assembled into one string.
void terminal_render_rows(Terminal *t);
int terminal_emit_frame(Terminal *t, FILE *out);
void terminal_enforce_framerate(Terminal *t);
void terminal_reset_canvas_area(Terminal *t, FILE *out);
// True when a resize has settled and would move the layout (tty-only callers).
bool terminal_resize_settled(Terminal *t);

// shutil.get_terminal_size semantics: COLUMNS/LINES win; else the tty; else 80x24.
void terminal_get_dimensions(int64_t *width, int64_t *height);

#endif
