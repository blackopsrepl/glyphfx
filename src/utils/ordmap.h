// Insertion-ordered string-keyed map. Python dict iteration order is behavior
// in several places (Motion.paths, Animation.scenes, gradient mappings), so
// these maps keep insertion order and replace values in place on re-insert.
#ifndef GLYPHFX_ORDMAP_H
#define GLYPHFX_ORDMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *key;      // owned
    uint64_t hash;  // cached hash of key, compared before strcmp
    void *value;
} OrdEntry;

typedef struct {
    OrdEntry *entries;
    size_t len;
    size_t cap;
} OrdMap;

void om_init(OrdMap *m);
void om_free(OrdMap *m);
size_t om_len(const OrdMap *m);
bool om_contains(const OrdMap *m, const char *key);
// Index of key, or -1.
long om_slot(const OrdMap *m, const char *key);
void *om_get(const OrdMap *m, const char *key);
// Inserts or replaces, preserving position. Takes ownership of value.
void om_insert(OrdMap *m, const char *key, void *value);
void om_set_at(OrdMap *m, size_t slot, void *value);
const char *om_key_at(const OrdMap *m, size_t slot);
void *om_value_at(const OrdMap *m, size_t slot);
void om_clear(OrdMap *m);

#endif
