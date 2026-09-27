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
const char *strtab_name(uint32_t handle);

#endif
