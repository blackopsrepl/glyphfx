#include "utils/strtab.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "utils/strhash.h"

typedef struct {
    const char *s;
    uint32_t handle;
} StrTabSlot;

static StrTabSlot *g_tab;
static size_t g_tab_cap;    // power of two
size_t g_strtab_used;
static bool g_tab_atexit;
InternedName *g_strtab_names;
static size_t g_strtab_names_cap;

static void strtab_free(void) {
    for (size_t i = 0; i < g_tab_cap; i++) {
        if (g_tab[i].s) {
            free((void *)(g_tab[i].s - STRTAB_HEADER));
        }
    }
    free(g_tab);
    free(g_strtab_names);
    g_strtab_names = NULL;
    g_strtab_names_cap = 0;
    g_tab = NULL;
    g_tab_cap = 0;
    g_strtab_used = 0;
}

static void strtab_grow(void) {
    size_t cap = g_tab_cap ? g_tab_cap * 2 : 64;
    StrTabSlot *grown = calloc(cap, sizeof(StrTabSlot));
    if (!grown) {
        return;
    }
    size_t mask = cap - 1;
    for (size_t i = 0; i < g_tab_cap; i++) {
        const char *s = g_tab[i].s;
        if (!s) {
            continue;
        }
        size_t j = (size_t)strtab_interned_hash(s) & mask;
        while (grown[j].s) {
            j = (j + 1) & mask;
        }
        grown[j] = g_tab[i];
    }
    free(g_tab);
    g_tab = grown;
    g_tab_cap = cap;
}

static StrTabSlot *intern_slot(const char *s) {
    if (!s) {
        return NULL;
    }
    if (!g_tab_atexit) {
        atexit(strtab_free);
        g_tab_atexit = true;
    }
    if ((g_strtab_used + 1) * 2 >= g_tab_cap) {
        strtab_grow();
        if (!g_tab) {
            return NULL;
        }
    }
    size_t mask = g_tab_cap - 1;
    uint64_t hash = str_hash64(s);
    size_t i = (size_t)hash & mask;
    while (g_tab[i].s) {
        if (g_tab[i].s == s || strcmp(g_tab[i].s, s) == 0) {
            return &g_tab[i];
        }
        i = (i + 1) & mask;
    }
    size_t len = strlen(s);
    // The handle sits in a header before the bytes, so an interned pointer
    // yields its handle with one load (strtab_interned_handle).
    char *block = malloc(STRTAB_HEADER + len + 1);
    if (!block) {
        return NULL;
    }
    char *copy = block + STRTAB_HEADER;
    memcpy(copy, s, len + 1);
    if (g_strtab_used == g_strtab_names_cap) {
        size_t cap = g_strtab_names_cap ? g_strtab_names_cap * 2 : 64;
        InternedName *grown = realloc(g_strtab_names, cap * sizeof(*grown));
        if (!grown) {
            free(block);
            return NULL;
        }
        g_strtab_names = grown;
        g_strtab_names_cap = cap;
    }
    uint32_t handle = (uint32_t)(g_strtab_used + 1);
    memcpy(block, &hash, sizeof(hash));
    memcpy(block + 8, &handle, sizeof(handle));
    g_tab[i].s = copy;
    g_tab[i].handle = handle;
    g_strtab_names[g_strtab_used] = (InternedName){copy, len};
    g_strtab_used++;
    return &g_tab[i];
}

const char *strtab_intern(const char *s) {
    StrTabSlot *slot = intern_slot(s);
    return slot ? slot->s : NULL;
}

uint32_t strtab_handle(const char *s) {
    StrTabSlot *slot = intern_slot(s);
    return slot ? slot->handle : 0;
}

uint32_t *strtab_handles(char *const *names, size_t n) {
    uint32_t *out = malloc((n ? n : 1) * sizeof(*out));
    if (!out) {
        return NULL;
    }
    for (size_t i = 0; i < n; i++) {
        out[i] = strtab_handle(names[i]);
    }
    return out;
}
