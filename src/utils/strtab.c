#include "utils/strtab.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "utils/strhash.h"

typedef struct {
    const char *s;
} StrTabSlot;

static StrTabSlot *g_tab;
static size_t g_tab_cap;    // power of two
static size_t g_tab_used;
static bool g_tab_atexit;

static void strtab_free(void) {
    for (size_t i = 0; i < g_tab_cap; i++) {
        free((void *)g_tab[i].s);
    }
    free(g_tab);
    g_tab = NULL;
    g_tab_cap = 0;
    g_tab_used = 0;
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
        size_t j = (size_t)str_hash64(s) & mask;
        while (grown[j].s) {
            j = (j + 1) & mask;
        }
        grown[j].s = s;
    }
    free(g_tab);
    g_tab = grown;
    g_tab_cap = cap;
}

const char *strtab_intern(const char *s) {
    if (!s) {
        return NULL;
    }
    if (!g_tab_atexit) {
        atexit(strtab_free);
        g_tab_atexit = true;
    }
    if ((g_tab_used + 1) * 2 >= g_tab_cap) {
        strtab_grow();
        if (!g_tab) {
            return NULL;
        }
    }
    size_t mask = g_tab_cap - 1;
    size_t i = (size_t)str_hash64(s) & mask;
    while (g_tab[i].s) {
        if (strcmp(g_tab[i].s, s) == 0) {
            return g_tab[i].s;
        }
        i = (i + 1) & mask;
    }
    size_t len = strlen(s);
    char *copy = malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, s, len + 1);
    g_tab[i].s = copy;
    g_tab_used++;
    return copy;
}
