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

// FNV-1a's low bits correlate for short numeric keys ("0".."706" in rings), so
// probe positions pass through a murmur3-style finalizer first.
static inline uint64_t om_probe_hash(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

// Capacity of the index table for a map of `n` entries: a power of two that
// keeps the load factor at or under 50%. Derived from len on every use so the
// index needs no header of its own.
static size_t om_idx_cap(size_t n) {
    size_t cap = 32;
    while (cap < n * 2) {
        cap <<= 1;
    }
    return cap;
}

// The index table lives in the tail of the heap block, behind the entry array,
// sized for the block's full entry capacity.
static uint32_t *om_index(const OrdMap *m) {
    return (uint32_t *)((const char *)m->heap + m->cap * sizeof(OrdEntry));
}

static void om_index_put(uint32_t *idx, size_t idx_cap, const OrdEntry *a, size_t slot) {
    size_t i = (size_t)om_probe_hash(a[slot].hash) & (idx_cap - 1);
    while (idx[i] != 0) {
        i = (i + 1) & (idx_cap - 1);
    }
    idx[i] = (uint32_t)(slot + 1);
}

// Out-of-line on purpose: inlining the probe beside the linear scan doubles
// om_slot's footprint and costs the small-map path more than the big-map path
// saves. Indexed lookups are rare relative to linear ones.
__attribute__((noinline)) static long om_slot_indexed(const OrdMap *m, const char *key, uint64_t h) {
    const OrdEntry *a = om_carr(m);
    size_t idx_cap = om_idx_cap(m->len);
    size_t mask = idx_cap - 1;
    size_t i = (size_t)om_probe_hash(h) & mask;
    const uint32_t *idx = om_index(m);
    for (;;) {
        uint32_t s = idx[i];
        if (s == 0) {
            return -1;
        }
        const OrdEntry *e = &a[s - 1];
        if (e->hash == h && strcmp(e->key, key) == 0) {
            return (long)(s - 1);
        }
        i = (i + 1) & mask;
    }
}

static void om_index_rebuild(OrdMap *m) {
    uint32_t *idx = om_index(m);
    memset(idx, 0, om_idx_cap(m->cap) * sizeof(uint32_t));
    OrdEntry *a = om_arr(m);
    size_t idx_cap = om_idx_cap(m->len);
    for (size_t s = 0; s < m->len; s++) {
        om_index_put(idx, idx_cap, a, s);
    }
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
    // len alone gates the index: the inline buffer holds OM_INLINE entries, so
    // any map this large already lives on the heap. The hint keeps the linear
    // scan (the overwhelmingly common case) first in layout.
    if (__builtin_expect(m->len >= OM_INDEX_MIN, 0)) {
        return om_slot_indexed(m, key, h);
    }
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
        // The block carries the index tail behind the entries, sized for the
        // full capacity, so enabling the index later costs no reallocation.
        size_t bytes = cap * sizeof(OrdEntry) + om_idx_cap(cap) * sizeof(uint32_t);
        if (m->heap) {
            OrdEntry *grown = realloc(m->heap, bytes);
            if (!grown) {
                return;
            }
            m->heap = grown;
        } else {
            OrdEntry *grown = malloc(bytes);
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
    if (m->len >= OM_INDEX_MIN) {
        // Slot numbers survive entry-array reallocs, so a full rebuild is only
        // needed when the index comes into existence or widens.
        if (m->len - 1 < OM_INDEX_MIN || om_idx_cap(m->len) != om_idx_cap(m->len - 1)) {
            om_index_rebuild(m);
        } else {
            om_index_put(om_index(m), om_idx_cap(m->len), a, m->len - 1);
        }
    }
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
    // Slot numbers stay valid for re-used slots; the len >= OM_INDEX_MIN gate
    // turns the index off, and it is rebuilt from scratch on the next crossing.
    m->len = 0;
}
