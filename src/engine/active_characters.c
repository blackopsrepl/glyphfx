#include "engine/active_characters.h"

#include <stdlib.h>
#include <string.h>

void ac_init(ActiveCharacters *ac) {
    ac->items = NULL;
    ac->len = 0;
    ac->cap = 0;
}

void ac_free(ActiveCharacters *ac) {
    free(ac->items);
    ac_init(ac);
}

size_t ac_len(const ActiveCharacters *ac) {
    return ac->len;
}

bool ac_is_empty(const ActiveCharacters *ac) {
    return ac->len == 0;
}

void ac_clear(ActiveCharacters *ac) {
    ac->len = 0;
}

static long ac_find(const ActiveCharacters *ac, CharId id) {
    size_t lo = 0;
    size_t hi = ac->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ac->items[mid] < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < ac->len && ac->items[lo] == id) {
        return (long)lo;
    }
    return -1;
}

bool ac_contains(const ActiveCharacters *ac, CharId id) {
    return ac_find(ac, id) >= 0;
}

bool ac_insert(ActiveCharacters *ac, CharId id) {
    size_t lo = 0;
    size_t hi = ac->len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ac->items[mid] < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < ac->len && ac->items[lo] == id) {
        return false;
    }
    if (ac->len == ac->cap) {
        size_t cap = ac->cap ? ac->cap * 2 : 16;
        CharId *grown = realloc(ac->items, cap * sizeof(CharId));
        if (!grown) {
            return false;
        }
        ac->items = grown;
        ac->cap = cap;
    }
    if (ac->len > lo) {
        memmove(ac->items + lo + 1, ac->items + lo, (ac->len - lo) * sizeof(CharId));
    }
    ac->items[lo] = id;
    ac->len++;
    return true;
}

bool ac_remove(ActiveCharacters *ac, CharId id) {
    long idx = ac_find(ac, id);
    if (idx < 0) {
        return false;
    }
    if (ac->len > (size_t)idx + 1) {
        memmove(ac->items + idx, ac->items + idx + 1, (ac->len - (size_t)idx - 1) * sizeof(CharId));
    }
    ac->len--;
    return true;
}

void ac_retain(ActiveCharacters *ac, bool (*keep)(CharId id, void *ctx), void *ctx) {
    size_t out = 0;
    for (size_t i = 0; i < ac->len; i++) {
        if (keep(ac->items[i], ctx)) {
            ac->items[out++] = ac->items[i];
        }
    }
    ac->len = out;
}

void ac_snapshot(const ActiveCharacters *ac, CharId **out, size_t *out_len) {
    CharId *copy = malloc((ac->len ? ac->len : 1) * sizeof(CharId));
    if (ac->len) {
        memcpy(copy, ac->items, ac->len * sizeof(CharId));
    }
    *out = copy;
    *out_len = ac->len;
}

void ac_extend(ActiveCharacters *ac, const CharId *ids, size_t n) {
    for (size_t i = 0; i < n; i++) {
        ac_insert(ac, ids[i]);
    }
}
