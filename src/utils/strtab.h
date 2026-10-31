// Global string intern table. Waypoint identities are cloned constantly -
// every path activation clones the origin and first waypoints into segments -
// so their id strings are interned once and shared by pointer. Interned
// strings are immutable and live for the process lifetime; freeing them
// individually is a bug, the table owns every byte and releases at exit.
#ifndef GLYPHFX_STRTAB_H
#define GLYPHFX_STRTAB_H

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

#endif
