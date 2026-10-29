#include "engine/active_characters.h"

#include <stdlib.h>
#include <string.h>

#include "engine/ctx.h"
#include "engine/doze.h"

// Only the engine's own active set holds dozers; effects also use this type
// for scratch sets, whose changes must not end a doze.
static bool doze_owns_set(const ActiveCharacters *ac) {
    return g_doze && g_doze->live && ac == &g_doze->ctx->active_characters;
}

void ac_init(ActiveCharacters *ac) {
    ac->bits = NULL;
    ac->nwords = 0;
    ac->len = 0;
    ac->lo = 0;
    ac->hi = 0;
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
    if (doze_owns_set(ac)) {
        doze_wake_all();
    }
    if (ac->bits) {
        memset(ac->bits, 0, ac->nwords * sizeof(uint64_t));
    }
    ac->len = 0;
    ac->lo = 0;
    ac->hi = 0;
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
    if (ac->hi <= ac->lo) {
        ac->lo = word;
        ac->hi = word + 1;
    } else {
        if (word < ac->lo) {
            ac->lo = word;
        }
        if (word >= ac->hi) {
            ac->hi = word + 1;
        }
    }
    return true;
}

bool ac_remove(ActiveCharacters *ac, CharId id) {
    if (doze_owns_set(ac)) {
        doze_wake(id);
    }
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

// Drop the members `keep` rejects, skipping those set in `skip`, then narrow
// the window to the words still holding members.
static void retain_window(ActiveCharacters *ac, const uint64_t *skip, size_t skip_words,
                          bool (*keep)(CharId id, void *ctx), void *ctx) {
    size_t lo = ac->lo;
    size_t hi = ac->hi < ac->nwords ? ac->hi : ac->nwords;
    for (size_t w = lo; w < hi; w++) {
        uint64_t bits = ac->bits[w];
        if (w < skip_words) {
            bits &= ~skip[w];
        }
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
    // keep() may have inserted members anywhere; the window already covers them
    lo = ac->lo;
    hi = ac->hi < ac->nwords ? ac->hi : ac->nwords;
    while (lo < hi && ac->bits[lo] == 0) {
        lo++;
    }
    while (hi > lo && ac->bits[hi - 1] == 0) {
        hi--;
    }
    ac->lo = lo < hi ? lo : 0;
    ac->hi = lo < hi ? hi : 0;
}

void ac_retain(ActiveCharacters *ac, bool (*keep)(CharId id, void *ctx), void *ctx) {
    retain_window(ac, NULL, 0, keep, ctx);
}

void ac_retain_except(ActiveCharacters *ac, const uint64_t *skip, size_t skip_words,
                      bool (*keep)(CharId id, void *ctx), void *ctx) {
    retain_window(ac, skip, skip_words, keep, ctx);
}

void ac_snapshot_into(const ActiveCharacters *ac, CharId *dst) {
    size_t k = 0;
    size_t hi = ac->hi < ac->nwords ? ac->hi : ac->nwords;
    for (size_t w = ac->lo; w < hi; w++) {
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
