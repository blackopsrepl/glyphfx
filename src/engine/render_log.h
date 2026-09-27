// Ordered renderer mutation records and the hooks that append them.
//
// The asm renderer (asm/engine/render.asm) keeps its cell grid incrementally:
// every mutation that can change what a cell shows appends an 8-byte record
// (kind, slot, value) to the frame's change log, and the renderer replays the
// log instead of walking every visible character. Each record carries the
// value at mutation time - a move records its destination cell, a visual
// change records the new handle - so a character that changes more than once
// between frames still replays exactly.
//
// Logging is only enabled while the grid is being maintained by replay; when
// the renderer is repainting (many changes per frame, or the first frames) the
// hooks return immediately.
#ifndef GLYPHFX_RENDER_LOG_H
#define GLYPHFX_RENDER_LOG_H

#include <stdbool.h>
#include <stdint.h>

#include "engine/character.h"
#include "utils/geometry.h"

enum {
    RLOG_MOVE = 0,    // value = destination cell, or UINT32_MAX when off-canvas
    RLOG_HANDLE = 1,  // value = visual version
    RLOG_LAYER = 2,   // value = layer
};

// A record packs the slot in the low 30 bits, the kind in bits 30-31, and the
// value in the high 32 bits, matching the asm's 8-byte entries.
#define RLOG_SLOT_MASK 0x3fffffffu

typedef struct {
    uint64_t *buf;
    uint32_t len;
    uint32_t cap;    // allocated entries
    uint32_t limit;  // per-frame append cap; hitting it marks overflow
    bool on;
    bool overflow;
} RenderLog;

extern RenderLog g_rlog;

void render_log_grow(void);
// Bind the terminal whose grid the renderer maintains. Moves need its canvas
// geometry to turn a coordinate into a cell.
struct Terminal;
void render_log_bind(struct Terminal *t);

static inline void rlog_push(CharId slot, uint32_t kind, uint32_t value) {
    if (!g_rlog.on) {
        return;
    }
    if (g_rlog.len == g_rlog.limit) {
        g_rlog.overflow = true;
        g_rlog.on = false;
        return;
    }
    if (g_rlog.len == g_rlog.cap) {
        render_log_grow();
        if (g_rlog.len == g_rlog.cap) {
            return;
        }
    }
    g_rlog.buf[g_rlog.len++] =
        (uint64_t)(uint32_t)slot | ((uint64_t)kind << 30) | ((uint64_t)value << 32);
}

// Mutation hooks. The enabled check is inlined so a disabled mutation costs a
// load and a branch rather than a call; the out-of-line part does the work.
void renderer_move_impl(CharId id, Coord coord);
void renderer_handle_impl(CharId id, uint32_t version);
void renderer_layer_impl(CharId id, int64_t layer);

static inline void renderer_move(CharId id, Coord coord) {
    if (g_rlog.on) {
        renderer_move_impl(id, coord);
    }
}

static inline void renderer_handle(CharId id, uint32_t version) {
    if (g_rlog.on) {
        renderer_handle_impl(id, version);
    }
}

static inline void renderer_layer(CharId id, int64_t layer) {
    if (g_rlog.on) {
        renderer_layer_impl(id, layer);
    }
}

#endif
