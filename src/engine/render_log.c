#include "engine/render_log.h"

#include <stdlib.h>

#include "engine/terminal.h"

RenderLog g_rlog;

static Terminal *g_target;

void render_log_bind(Terminal *t) {
    g_target = t;
}

void render_log_grow(void) {
    uint32_t cap = g_rlog.cap ? g_rlog.cap * 2 : 4096;
    uint64_t *grown = realloc(g_rlog.buf, (size_t)cap * sizeof(uint64_t));
    if (!grown) {
        return;
    }
    g_rlog.buf = grown;
    g_rlog.cap = cap;
}

// The cell a character currently maps to, or -1 when hidden or off the visible
// canvas. Bounds match the renderer's paint walk.
static int32_t cell_for(const Terminal *t, CharId id, Coord coord) {
    size_t width = t->visible_right > 0 ? (size_t)t->visible_right : 0;
    if (width == 0 || !t->arena.items[id].is_visible) {
        return -1;
    }
    int64_t row = coord.row + t->canvas_row_offset;
    int64_t col = coord.column + t->canvas_column_offset;
    if (t->visible_bottom <= row && row <= t->visible_top && t->visible_left <= col &&
        col <= t->visible_right) {
        return (int32_t)((row - 1) * (int64_t)width + (col - 1));
    }
    return -1;
}

void renderer_move_impl(CharId id, Coord coord) {
    Terminal *t = g_target;
    if (!t || id < 0 || (size_t)id >= t->arena.len) {
        return;
    }
    int32_t cell = cell_for(t, id, coord);
    // Always record the move. The grid's per-cell mirror is only advanced by
    // the replay, so a character that moves twice (or returns to its replayed
    // cell) in one frame must still be recorded or the grid would go stale.
    if ((size_t)id < t->grid_chars_len && t->cell_of_char[id] < 0 && cell >= 0) {
        // Entering a cell: the layer and visual matter from here on, so they
        // are logged before the move.
        rlog_push(id, RLOG_LAYER, (uint32_t)t->arena.items[id].layer);
        rlog_push(id, RLOG_HANDLE, t->arena.items[id].animation.current_visual);
    }
    rlog_push(id, RLOG_MOVE, (uint32_t)cell);
}

void renderer_handle_impl(CharId id, uint32_t version) {
    Terminal *t = g_target;
    if (!t || id < 0 || (size_t)id >= t->arena.len) {
        return;
    }
    if (cell_for(t, id, t->arena.items[id].motion.current_coord) < 0) {
        return;
    }
    rlog_push(id, RLOG_HANDLE, version);
}

void renderer_layer_impl(CharId id, int64_t layer) {
    Terminal *t = g_target;
    if (!t || id < 0 || (size_t)id >= t->arena.len) {
        return;
    }
    if (cell_for(t, id, t->arena.items[id].motion.current_coord) < 0) {
        return;
    }
    rlog_push(id, RLOG_LAYER, (uint32_t)layer);
}
