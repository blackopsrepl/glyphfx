// Global string intern table. Waypoint identities are cloned constantly -
// every path activation clones the origin and first waypoints into segments -
// so their id strings are interned once and shared by pointer. Interned
// strings are immutable and live for the process lifetime; freeing them
// individually is a bug, the table owns every byte and releases at exit.
#ifndef GLYPHFX_STRTAB_H
#define GLYPHFX_STRTAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Returns the shared copy of s (NULL passes through).
const char *strtab_intern(const char *s);
// A dense identity for a name, assigned in first-seen order and stable for the
// process lifetime; 0 means "no name". Event keys compare handles instead of
// strings (plan 7.2), so a lookup is an integer compare.
uint32_t strtab_handle(const char *s);
// Handles for n names, resolved once (heap array, caller frees; NULL on
// allocation failure). Effects keep these instead of re-interning per frame.
uint32_t *strtab_handles(char *const *names, size_t n);

typedef struct {
    const char *text;
    size_t len;
} InternedName;
// Handle-indexed names (handle - 1); exposed so handle lookups inline.
extern InternedName *g_strtab_names;
extern size_t g_strtab_used;

static inline const char *strtab_name(uint32_t handle) {
    return handle && handle <= g_strtab_used ? g_strtab_names[handle - 1].text : NULL;
}
// Byte length recorded when the name was interned, without hashing or scanning.
static inline size_t strtab_length(uint32_t handle) {
    return handle && handle <= g_strtab_used ? g_strtab_names[handle - 1].len : 0;
}

// Every interned name is preceded by a header: its str_hash64, then its handle.
#define STRTAB_HEADER 16
// The handle of a pointer strtab_intern returned (and only such a pointer;
// NULL gives 0): one load, no hashing.
static inline uint32_t strtab_interned_handle(const char *interned) {
    uint32_t handle = 0;
    if (interned) {
        __builtin_memcpy(&handle, interned - STRTAB_HEADER + 8, sizeof(handle));
    }
    return handle;
}
// The str_hash64 of an interned name, recorded when it was interned.
static inline uint64_t strtab_interned_hash(const char *interned) {
    uint64_t hash;
    __builtin_memcpy(&hash, interned - STRTAB_HEADER, sizeof(hash));
    return hash;
}

typedef struct {
    char *base;
    size_t used;
    size_t cap;
} StrTabChunk;
// The chunks holding every interned name (a handful); exported for the check.
extern StrTabChunk *g_strtab_chunks;
extern size_t g_strtab_chunks_len;

// Whether p is a name strtab_intern returned. Effects intern their names
// once and pass the pointers; every name API then skips hashing. A pointer
// into a chunk qualifies only when its header's handle names it exactly, so
// a pointer into the middle of a name never passes for one.
static inline bool strtab_owns(const char *p) {
    for (size_t i = 0; i < g_strtab_chunks_len; i++) {
        const StrTabChunk *c = &g_strtab_chunks[i];
        uintptr_t off = (uintptr_t)p - (uintptr_t)c->base;
        if (off < c->used) {
            if (off < STRTAB_HEADER) {
                return false;
            }
            uint32_t handle;
            __builtin_memcpy(&handle, p - STRTAB_HEADER + 8, sizeof(handle));
            return handle && handle <= g_strtab_used && g_strtab_names[handle - 1].text == p;
        }
    }
    return false;
}

// This program's read-only segments: a pointer into them is a literal whose
// bytes never change at that address, so its interned name is cached by
// address (strtab_literal).
typedef struct {
    uintptr_t lo, hi;
} RoRange;
extern RoRange g_strtab_ro[8];
extern size_t g_strtab_ro_len;

// Literal address -> interned name, open addressing on the full pointer
// (see strtab.c). The first probe is inlined; strtab_literal() handles a
// collision chain or a first sighting.
typedef struct {
    const char *lit;
    const char *interned;
} LitEntry;
extern LitEntry *g_strtab_lit;
extern size_t g_strtab_lit_cap;  // power of two, or 0 before the first literal

static inline size_t strtab_lit_index(const char *p, size_t cap) {
    return (size_t)(((uint64_t)(uintptr_t)p * UINT64_C(0x9E3779B97F4A7C15)) >> 32) & (cap - 1);
}

const char *strtab_literal(const char *p);

static inline const char *strtab_literal_cached(const char *p) {
    if (g_strtab_lit_cap) {
        const LitEntry *e = &g_strtab_lit[strtab_lit_index(p, g_strtab_lit_cap)];
        if (e->lit == p) {
            return e->interned;
        }
    }
    return strtab_literal(p);
}

// The interned name for p without hashing when p is a read-only literal or
// already interned; NULL for any other text (the caller hashes it).
static inline const char *strtab_resolve(const char *p) {
    uintptr_t a = (uintptr_t)p;
    for (size_t i = 0; i < g_strtab_ro_len; i++) {
        if (a - g_strtab_ro[i].lo < g_strtab_ro[i].hi - g_strtab_ro[i].lo) {
            return strtab_literal_cached(p);
        }
    }
    return p && strtab_owns(p) ? p : NULL;
}

#endif
