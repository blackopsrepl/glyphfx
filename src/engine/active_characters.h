// Ascending-id set of active arena characters. The reference uses a packed
// bitmap that iterates in ascending id order; a sorted vector has the same
// observable order. CharId order equals character_id order by construction.
#ifndef GLYPHFX_ACTIVE_CHARACTERS_H
#define GLYPHFX_ACTIVE_CHARACTERS_H

#include <stdbool.h>
#include <stddef.h>

#include "engine/character.h"

typedef struct {
    CharId *items;
    size_t len;
    size_t cap;
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
// Ascending snapshot.
void ac_snapshot(const ActiveCharacters *ac, CharId **out, size_t *out_len);
void ac_extend(ActiveCharacters *ac, const CharId *ids, size_t n);

#endif
