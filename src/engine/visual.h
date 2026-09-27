// The visual pool: an interned, byte-addressed store of formatted characters.
//
// A visual is formatted once - SGR attributes, foreground and background,
// symbol, reset - and interned by the header that determines those bytes. A
// visual is then a 32-bit handle: the offset of its bytes in the pool in the
// low 24 bits, their length in the high 8. The renderer never formats,
// allocates or frees a visual; a cell is one bounded copy from a handle.
//
// Each visual is preceded by a 32-byte header holding what the visual *is*,
// for effects that read it back (the reference's CharacterVisual.symbol and
// .colors): the packed symbol, the logical foreground and background colours,
// and the attribute bits. Under --no-color the bytes carry no colour but the
// header keeps the logical ones, so effect logic sees what Rust sees. The
// header is the interning key.
//
// Every pooled visual is readable for 128 bytes from its start (the pool keeps
// that much slack after each one), so a copy may overrun the visual's length
// without leaving the pool. Bytes are never mutated: a restyle with no change
// costs nothing, and any real change is a fresh interned entry.
#ifndef GLYPHFX_VISUAL_H
#define GLYPHFX_VISUAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/graphics.h"

struct VisualParams;

typedef uint32_t VisualHandle;

// Attribute bits, in VisualParams' order. `dim` is stored but never emitted, so
// it has no bit; it is not part of the interning key.
enum {
    VF_BOLD = 1,
    VF_ITALIC = 2,
    VF_UNDERLINE = 4,
    VF_BLINK = 8,
    VF_REVERSE = 16,
    VF_HIDDEN = 32,
    VF_STRIKE = 64,
};

#define VISUAL_MAX 128        // longest visual the pool accepts, bytes
#define VISUAL_HEADER 48      // bytes of header before the formatted bytes
#define VISUAL_SLACK 128      // readable bytes guaranteed past a visual's start
#define VISUAL_OFFSET_MASK 0x00ffffffu
#define VISUAL_LEN_SHIFT 24

void visual_init(void);
// Releases the pool and intern table. Called between runs (a resize restart).
void visual_free(void);

// Interns the appearance and returns its handle. The bytes are exactly what
// the previous vis_format produced for these params. Returns 0 when the pool is
// exhausted or the symbol exceeds VISUAL_MAX bytes.
VisualHandle visual_make(const char *symbol, const struct VisualParams *params);

// The visual's formatted bytes and length, from a handle. Never NULL for a
// nonzero handle; the bytes are readable for VISUAL_SLACK bytes.
static inline const char *visual_bytes(VisualHandle h) {
    extern const char *g_visual_pool;
    return g_visual_pool + (h & VISUAL_OFFSET_MASK);
}
static inline uint32_t visual_len(VisualHandle h) {
    return h >> VISUAL_LEN_SHIFT;
}

// The header fields, for effects that read a visual back. `symbol` is the
// interned input symbol; the colours are the logical ones (see above).
const char *visual_symbol(VisualHandle h);
bool visual_colors(VisualHandle h, Color *fg, Color *bg, uint8_t *attrs);

#endif
