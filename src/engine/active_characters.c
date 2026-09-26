#include "engine/active_characters.h"

#include <stdlib.h>
#include <string.h>

void ac_init(ActiveCharacters *ac) {
    ac->bits = NULL;
    ac->nwords = 0;
    ac->len = 0;
}

void ac_free(ActiveCharacters *ac) {
    free(ac->bits);
    ac_init(ac);
}

size_t ac_len(const ActiveCharacters *ac) {
    return ac->len;
}

bool ac_is_empty(const ActiveCharacters *ac) {
    return ac->len == 0;
}

void ac_clear(ActiveCharacters *ac) {
    if (ac->bits) {
        memset(ac->bits, 0, ac->nwords * sizeof(uint64_t));
    }
    ac->len = 0;
}

static void ac_ensure(ActiveCharacters *ac, CharId id) {
    size_t word = (size_t)id >> 6;
    if (word < ac->nwords) {
        return;
    }
    size_t cap = ac->nwords ? ac->nwords : 64;
    while (cap <= word) {
        cap *= 2;
    }
    uint64_t *grown = realloc(ac->bits, cap * sizeof(uint64_t));
    if (!grown) {
        return;
    }
    memset(grown + ac->nwords, 0, (cap - ac->nwords) * sizeof(uint64_t));
    ac->bits = grown;
    ac->nwords = cap;
}

bool ac_contains(const ActiveCharacters *ac, CharId id) {
    size_t word = (size_t)id >> 6;
    if (word >= ac->nwords) {
        return false;
    }
    return (ac->bits[word] >> (id & 63)) & 1u;
}

bool ac_insert(ActiveCharacters *ac, CharId id) {
    ac_ensure(ac, id);
    size_t word = (size_t)id >> 6;
    if (word >= ac->nwords) {
        return false;
    }
    uint64_t mask = 1ULL << (id & 63);
    if (ac->bits[word] & mask) {
        return false;
    }
    ac->bits[word] |= mask;
    ac->len++;
    return true;
}

bool ac_remove(ActiveCharacters *ac, CharId id) {
    size_t word = (size_t)id >> 6;
    if (word >= ac->nwords) {
        return false;
    }
    uint64_t mask = 1ULL << (id & 63);
    if (!(ac->bits[word] & mask)) {
        return false;
    }
    ac->bits[word] &= ~mask;
    ac->len--;
    return true;
}

void ac_retain(ActiveCharacters *ac, bool (*keep)(CharId id, void *ctx), void *ctx) {
    for (size_t w = 0; w < ac->nwords; w++) {
        uint64_t bits = ac->bits[w];
        while (bits) {
            unsigned b = (unsigned)__builtin_ctzll(bits);
            CharId id = (CharId)((w << 6) | b);
            if (!keep(id, ctx)) {
                ac->bits[w] &= ~(1ULL << b);
                ac->len--;
            }
            bits &= bits - 1;
        }
    }
}

void ac_snapshot_into(const ActiveCharacters *ac, CharId *dst) {
    size_t k = 0;
    for (size_t w = 0; w < ac->nwords; w++) {
        uint64_t bits = ac->bits[w];
        while (bits) {
            unsigned b = (unsigned)__builtin_ctzll(bits);
            dst[k++] = (CharId)((w << 6) | b);
            bits &= bits - 1;
        }
    }
}

void ac_snapshot(const ActiveCharacters *ac, CharId **out, size_t *out_len) {
    CharId *a = malloc((ac->len ? ac->len : 1) * sizeof(CharId));
    if (a) {
        ac_snapshot_into(ac, a);
    }
    *out = a;
    *out_len = ac->len;
}

void ac_extend(ActiveCharacters *ac, const CharId *ids, size_t n) {
    for (size_t i = 0; i < n; i++) {
        ac_insert(ac, ids[i]);
    }
}
