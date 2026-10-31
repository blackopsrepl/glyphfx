#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE  // dl_iterate_phdr
#endif

#include "utils/strtab.h"

#if defined(__linux__)
#include <link.h>
#endif

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
// Interned bytes live in a few large chunks, so whether a pointer is an
// interned name is a range check (strtab_owns) rather than a hash.
#define STRTAB_CHUNK ((size_t)64 * 1024)
StrTabChunk *g_strtab_chunks;
size_t g_strtab_chunks_len;
static size_t g_strtab_chunks_cap;
static void lit_cache_clear(void);
static void ro_init(void);

static char *chunk_alloc(size_t n) {
    n = (n + 7u) & ~(size_t)7u;  // keep every header 8-byte aligned
    StrTabChunk *last = g_strtab_chunks_len ? &g_strtab_chunks[g_strtab_chunks_len - 1] : NULL;
    if (!last || last->cap - last->used < n) {
        if (g_strtab_chunks_len == g_strtab_chunks_cap) {
            size_t cap = g_strtab_chunks_cap ? g_strtab_chunks_cap * 2 : 8;
            StrTabChunk *grown = realloc(g_strtab_chunks, cap * sizeof(*grown));
            if (!grown) {
                return NULL;
            }
            g_strtab_chunks = grown;
            g_strtab_chunks_cap = cap;
        }
        size_t cap = n > STRTAB_CHUNK ? n : STRTAB_CHUNK;
        char *base = malloc(cap);
        if (!base) {
            return NULL;
        }
        last = &g_strtab_chunks[g_strtab_chunks_len++];
        *last = (StrTabChunk){base, 0, cap};
    }
    char *out = last->base + last->used;
    last->used += n;
    return out;
}

static void strtab_free(void) {
    for (size_t i = 0; i < g_strtab_chunks_len; i++) {
        free(g_strtab_chunks[i].base);
    }
    free(g_strtab_chunks);
    g_strtab_chunks = NULL;
    g_strtab_chunks_len = 0;
    g_strtab_chunks_cap = 0;
    lit_cache_clear();
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
        ro_init();
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
    // The hash and handle sit in a header before the bytes, so an interned
    // pointer yields both with one load each.
    if (g_strtab_used == g_strtab_names_cap) {
        size_t cap = g_strtab_names_cap ? g_strtab_names_cap * 2 : 64;
        InternedName *grown = realloc(g_strtab_names, cap * sizeof(*grown));
        if (!grown) {
            return NULL;
        }
        g_strtab_names = grown;
        g_strtab_names_cap = cap;
    }
    char *block = chunk_alloc(STRTAB_HEADER + len + 1);
    if (!block) {
        return NULL;
    }
    char *copy = block + STRTAB_HEADER;
    memcpy(copy, s, len + 1);
    uint32_t handle = (uint32_t)(g_strtab_used + 1);
    memcpy(block, &hash, sizeof(hash));
    memcpy(block + 8, &handle, sizeof(handle));
    g_tab[i].s = copy;
    g_tab[i].handle = handle;
    g_strtab_names[g_strtab_used] = (InternedName){copy, len};
    g_strtab_used++;
    return &g_tab[i];
}

// --- read-only literals ---------------------------------------------------
// A pointer into this program's read-only segments is a string literal (or
// other constant) whose bytes can never change at that address. Such names
// are cached by address: an effect passing "glitch" pays the hash once, then
// one probe per call, as fx's names resolved at compile time do.

RoRange g_strtab_ro[8];
size_t g_strtab_ro_len;
static bool g_ro_ready;
static const char g_ro_marker[] = "glyphfx strtab";

#if defined(__linux__)
// Collects the non-writable PT_LOAD segments of the object holding this code
// (the executable, or the library glyphfx was linked into).
static int ro_collect(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    (void)data;
    uintptr_t marker = (uintptr_t)g_ro_marker;
    bool mine = false;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t lo = (uintptr_t)(info->dlpi_addr + ph->p_vaddr);
        if (ph->p_type == PT_LOAD && marker >= lo && marker < lo + ph->p_memsz) {
            mine = true;
        }
    }
    if (!mine) {
        return 0;
    }
    for (int i = 0; i < info->dlpi_phnum && g_strtab_ro_len < sizeof(g_strtab_ro) / sizeof(g_strtab_ro[0]); i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD && !(ph->p_flags & PF_W)) {
            uintptr_t lo = (uintptr_t)(info->dlpi_addr + ph->p_vaddr);
            g_strtab_ro[g_strtab_ro_len++] = (RoRange){lo, lo + ph->p_memsz};
        }
    }
    // The read-only segments usually sit together ahead of the writable one:
    // when no writable segment falls inside their span, the span is a single
    // range and the check one compare.
    if (g_strtab_ro_len > 1) {
        RoRange span = g_strtab_ro[0];
        for (size_t r = 1; r < g_strtab_ro_len; r++) {
            if (g_strtab_ro[r].lo < span.lo) span.lo = g_strtab_ro[r].lo;
            if (g_strtab_ro[r].hi > span.hi) span.hi = g_strtab_ro[r].hi;
        }
        bool clean = true;
        for (int i = 0; i < info->dlpi_phnum; i++) {
            const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
            uintptr_t lo = (uintptr_t)(info->dlpi_addr + ph->p_vaddr);
            if (ph->p_type == PT_LOAD && (ph->p_flags & PF_W) && lo < span.hi && lo + ph->p_memsz > span.lo) {
                clean = false;
            }
        }
        if (clean) {
            g_strtab_ro[0] = span;
            g_strtab_ro_len = 1;
        }
    }
    return 1;
}
#endif

// Collected once, before the first name is interned: until then no pointer
// can name an interned string, so resolving needs no ranges.
static void ro_init(void) {
    if (!g_ro_ready) {
        g_ro_ready = true;
#if defined(__linux__)
        dl_iterate_phdr(ro_collect, NULL);
#endif
    }
}

// Literal address -> interned name. Open addressing with linear probing,
// keyed on the full pointer and never evicting: the literal set is bounded
// by glyphfx's own read-only segment, so each literal hashes exactly once
// per process whatever addresses the linker gave it. LitEntry and the
// inlined first probe live in strtab.h.
LitEntry *g_strtab_lit;
size_t g_strtab_lit_cap;
static size_t g_lit_len;

static void lit_cache_clear(void) {
    free(g_strtab_lit);
    g_strtab_lit = NULL;
    g_strtab_lit_cap = 0;
    g_lit_len = 0;
    g_strtab_ro_len = 0;
    g_ro_ready = false;
}

static bool lit_grow(void) {
    size_t cap = g_strtab_lit_cap ? g_strtab_lit_cap * 2 : 4096;
    LitEntry *table = calloc(cap, sizeof(LitEntry));
    if (!table) {
        return false;
    }
    for (size_t i = 0; i < g_strtab_lit_cap; i++) {
        if (g_strtab_lit[i].lit) {
            size_t j = strtab_lit_index(g_strtab_lit[i].lit, cap);
            while (table[j].lit) {
                j = (j + 1) & (cap - 1);
            }
            table[j] = g_strtab_lit[i];
        }
    }
    free(g_strtab_lit);
    g_strtab_lit = table;
    g_strtab_lit_cap = cap;
    return true;
}

const char *strtab_literal(const char *p) {
    size_t mask = g_strtab_lit_cap - 1;
    if (g_strtab_lit_cap) {
        for (size_t i = strtab_lit_index(p, g_strtab_lit_cap);; i = (i + 1) & mask) {
            if (g_strtab_lit[i].lit == p) {
                return g_strtab_lit[i].interned;
            }
            if (!g_strtab_lit[i].lit) {
                break;
            }
        }
    }
    StrTabSlot *slot = intern_slot(p);
    if (!slot) {
        return NULL;
    }
    // Load factor at most 1/4, so the inlined first probe nearly always decides.
    if ((g_lit_len + 1) * 4 > g_strtab_lit_cap && !lit_grow()) {
        return slot->s;  // uncached, still correct
    }
    mask = g_strtab_lit_cap - 1;
    size_t i = strtab_lit_index(p, g_strtab_lit_cap);
    while (g_strtab_lit[i].lit) {
        i = (i + 1) & mask;
    }
    g_strtab_lit[i].lit = p;
    g_strtab_lit[i].interned = slot->s;
    g_lit_len++;
    return slot->s;
}

const char *strtab_intern(const char *s) {
    const char *known = strtab_resolve(s);
    if (known) {
        return known;
    }
    StrTabSlot *slot = intern_slot(s);
    return slot ? slot->s : NULL;
}

uint32_t strtab_handle(const char *s) {
    const char *known = strtab_resolve(s);
    if (known) {
        return strtab_interned_handle(known);
    }
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
