#include "utils/ordmap.h"

#include <stdlib.h>
#include <string.h>

static char *dup_cstr(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, len + 1);
    return out;
}

// FNV-1a. Keys are short identifiers, so this is a few instructions and lets a
// lookup skip strcmp for every non-matching entry (the common case: a character
// asks a per-character map for a scene or path it does not have).
static uint64_t str_hash(const char *s) {
    uint64_t h = 1469598103934665603ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h;
}

void om_init(OrdMap *m) {
    m->entries = NULL;
    m->len = 0;
    m->cap = 0;
}

void om_free(OrdMap *m) {
    for (size_t i = 0; i < m->len; i++) {
        free(m->entries[i].key);
    }
    free(m->entries);
    om_init(m);
}

size_t om_len(const OrdMap *m) {
    return m->len;
}

long om_slot(const OrdMap *m, const char *key) {
    uint64_t h = str_hash(key);
    for (size_t i = 0; i < m->len; i++) {
        if (m->entries[i].hash == h && strcmp(m->entries[i].key, key) == 0) {
            return (long)i;
        }
    }
    return -1;
}

bool om_contains(const OrdMap *m, const char *key) {
    return om_slot(m, key) >= 0;
}

void *om_get(const OrdMap *m, const char *key) {
    long slot = om_slot(m, key);
    return slot < 0 ? NULL : m->entries[slot].value;
}

void om_insert(OrdMap *m, const char *key, void *value) {
    long slot = om_slot(m, key);
    if (slot >= 0) {
        m->entries[slot].value = value;
        return;
    }
    if (m->len == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : 8;
        OrdEntry *grown = realloc(m->entries, cap * sizeof(OrdEntry));
        if (!grown) {
            return;
        }
        m->entries = grown;
        m->cap = cap;
    }
    m->entries[m->len].key = dup_cstr(key);
    m->entries[m->len].hash = str_hash(key);
    m->entries[m->len].value = value;
    m->len++;
}

void om_set_at(OrdMap *m, size_t slot, void *value) {
    if (slot < m->len) {
        m->entries[slot].value = value;
    }
}

const char *om_key_at(const OrdMap *m, size_t slot) {
    return slot < m->len ? m->entries[slot].key : NULL;
}

void *om_value_at(const OrdMap *m, size_t slot) {
    return slot < m->len ? m->entries[slot].value : NULL;
}

void om_clear(OrdMap *m) {
    for (size_t i = 0; i < m->len; i++) {
        free(m->entries[i].key);
    }
    m->len = 0;
}
