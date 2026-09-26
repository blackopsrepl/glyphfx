// Ascending-id set of active arena characters. A packed bitmap, matching the
// reference: insert, remove and membership are O(1), iteration is ascending by
// construction, and there is no per-insert shifting (the sorted-vector version
// memmoved the tail, which dominated spotlight-style effects).
#ifndef GLYPHFX_ACTIVE_CHARACTERS_H
#define GLYPHFX_ACTIVE_CHARACTERS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine/character.h"

typedef struct {
    uint64_t *bits;
    size_t nwords;
    size_t len;  // number of set bits
} ActiveCharacters;

void ac_init(ActiveCharacters *ac);
void ac_free(ActiveCharacters *ac);
size_t ac_len(const ActiveCharacters *ac);
bool ac_is_empty(const ActiveCharacters *ac);
void ac_clear(ActiveCharacters *ac);
bool ac_contains(const ActiveCharacters *ac, CharId id);
// Returns true when newly inserted.
bool ac_insert(ActiveCharacters *ac, CharId id);
bool ac_remove(ActiveCharacters *ac, CharId id);
void ac_retain(ActiveCharacters *ac, bool (*keep)(CharId id, void *ctx), void *ctx);
// Ascending snapshot; caller frees.
void ac_snapshot(const ActiveCharacters *ac, CharId **out, size_t *out_len);
// Ascending snapshot into a caller buffer of at least ac_len() entries.
void ac_snapshot_into(const ActiveCharacters *ac, CharId *dst);
void ac_extend(ActiveCharacters *ac, const CharId *ids, size_t n);

#endif
