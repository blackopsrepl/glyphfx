#include "utils/ordmap.h"

#include <stdlib.h>
#include <string.h>

#include "utils/strhash.h"

static char *dup_cstr(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, len + 1);
    return out;
}

static OrdEntry *om_arr(OrdMap *m) {
    return m->heap ? m->heap : m->inline_entries;
}

static const OrdEntry *om_carr(const OrdMap *m) {
    return m->heap ? m->heap : m->inline_entries;
}

void om_init(OrdMap *m) {
    m->heap = NULL;
    m->len = 0;
    m->cap = OM_INLINE;
}

void om_free(OrdMap *m) {
    OrdEntry *a = om_arr(m);
    for (size_t i = 0; i < m->len; i++) {
        free(a[i].key);
    }
    free(m->heap);
    om_init(m);
}

size_t om_len(const OrdMap *m) {
    return m->len;
}

long om_slot(const OrdMap *m, const char *key) {
    const OrdEntry *a = om_carr(m);
    uint64_t h = str_hash64(key);
    for (size_t i = 0; i < m->len; i++) {
        if (a[i].hash == h && strcmp(a[i].key, key) == 0) {
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
    return slot < 0 ? NULL : om_carr(m)[slot].value;
}

void om_insert(OrdMap *m, const char *key, void *value) {
    long slot = om_slot(m, key);
    if (slot >= 0) {
        om_arr(m)[slot].value = value;
        return;
    }
    if (m->len == m->cap) {
        size_t cap = m->cap ? m->cap * 2 : OM_INLINE;
        if (m->heap) {
            OrdEntry *grown = realloc(m->heap, cap * sizeof(OrdEntry));
            if (!grown) {
                return;
            }
            m->heap = grown;
        } else {
            OrdEntry *grown = malloc(cap * sizeof(OrdEntry));
            if (!grown) {
                return;
            }
            memcpy(grown, m->inline_entries, m->len * sizeof(OrdEntry));
            m->heap = grown;
        }
        m->cap = cap;
    }
    OrdEntry *a = om_arr(m);
    a[m->len].key = dup_cstr(key);
    a[m->len].hash = str_hash64(key);
    a[m->len].value = value;
    m->len++;
}

void om_set_at(OrdMap *m, size_t slot, void *value) {
    if (slot < m->len) {
        om_arr(m)[slot].value = value;
    }
}

const char *om_key_at(const OrdMap *m, size_t slot) {
    return slot < m->len ? om_carr(m)[slot].key : NULL;
}

void *om_value_at(const OrdMap *m, size_t slot) {
    return slot < m->len ? om_carr(m)[slot].value : NULL;
}

void om_clear(OrdMap *m) {
    OrdEntry *a = om_arr(m);
    for (size_t i = 0; i < m->len; i++) {
        free(a[i].key);
    }
    m->len = 0;
}
