// The visual pool (see visual.h). This file mirrors the asm engine's
// engine/visual.asm: format once, intern by the header, address by offset.
#include "engine/visual.h"

#include "engine/animation.h"  // VisualParams

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/strtab.h"
#include "utils/ansi.h"

#if defined(__linux__)
#include <sys/mman.h>
#endif

// Offsets must fit the handle's 24 bits; the pool is one mapping that never
// moves, so a handle stays valid for the run.
#define POOL_LIMIT 0x00ffff00u
#define POOL_RESERVE (POOL_LIMIT + 4096u)
#define TABLE_INITIAL (1u << 15)

// Header, before a visual's bytes: [0,8) interned symbol pointer; [8] dim;
// [9] attribute bits; [12,44) the logical ColorPair, each colour packed as
// (is_xterm, xterm, hex_len, hex bytes, rgb). Zero-padded so the header is a
// deterministic interning key: equal headers mean equal bytes.
// Record layout: a 48-byte packed key (four 64-bit words, the interning key),
// then the readable logical header. The key is what the bytes are a function
// of, so equal keys mean equal bytes; the logical header keeps what the visual
// *is* for effects that read it back.
#define VK_WORDS 6
#define VK_BYTES (VK_WORDS * 8)
#define VK_END VK_BYTES
#define VH_SYMBOL VK_END      // u64 const char * (logical)
#define VH_DIM (VK_END + 8)   // u8
#define VH_ATTRS (VK_END + 9) // u8
#define VH_FG (VK_END + 12)   // u8 present, then a packed Color
#define VH_BG (VK_END + 28)   // u8 present, then a packed Color
#define VH_SIZE 96

const char *g_visual_pool;

typedef struct {
    uint32_t handle;  // low half: pool offset | len<<24; 0 = empty entry
    uint64_t hash;    // high half: the header's hash
} VisEntry;

static VisEntry *g_table;
static uint32_t g_table_mask;
static uint32_t g_table_used;
static uint32_t g_pool_top;  // next free offset (header-aligned)
static char *g_hdr;          // scratch header being keyed
static char *g_fmt;          // scratch formatted bytes
static size_t g_fmt_cap;
static bool g_ready;

static void visual_table_alloc(uint32_t capacity);

static void visual_oom(void) {
    fputs("glyphfx: out of memory (visual pool)\n", stderr);
    abort();
}

void visual_init(void) {
    if (g_ready) {
        return;
    }
#if defined(__linux__)
    void *p = mmap(NULL, POOL_RESERVE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        visual_oom();
    }
    g_visual_pool = p;
#else
    g_visual_pool = calloc(1, POOL_RESERVE);
    if (!g_visual_pool) {
        visual_oom();
    }
#endif
    visual_table_alloc(TABLE_INITIAL);
    g_hdr = calloc(1, VH_SIZE);
    if (!g_hdr) {
        visual_oom();
    }
    g_fmt_cap = 256;
    g_fmt = malloc(g_fmt_cap);
    if (!g_hdr || !g_fmt) {
        visual_oom();
    }
    g_ready = true;
}

void visual_free(void) {
    if (!g_ready) {
        return;
    }
#if defined(__linux__)
    munmap((void *)g_visual_pool, POOL_RESERVE);
#else
    free((void *)g_visual_pool);
#endif
    free(g_table);
    free(g_hdr);
    free(g_fmt);
    g_visual_pool = NULL;
    g_table = NULL;
    g_table_mask = 0;
    g_table_used = 0;
    g_pool_top = 0;
    g_ready = false;
}

static void visual_table_alloc(uint32_t capacity) {
    free(g_table);
    g_table = calloc(capacity, sizeof(*g_table));
    if (!g_table) {
        visual_oom();
    }
    g_table_mask = capacity - 1;
    g_table_used = 0;
}

// A code packs into two words: its kind/xterm/presence, and its full 16-byte
// hex buffer (a code's hex may hold more than 8 digits, and code_key_eq
// compares all 16 bytes). No field is dropped, so equal keys mean equal bytes.
static void pack_code(const ColorCode *c, bool has, uint64_t out[2]) {
    out[0] = 0;
    out[1] = 0;
    if (!has) {
        return;
    }
    out[0] = 1u | ((uint64_t)(uint8_t)c->kind << 1) | ((uint64_t)c->xterm << 9);
    memcpy(&out[1], c->hex, sizeof(c->hex));
}

// Four multiplies over four words, as the asm engine's VISUAL_HASH does; only
// probe order depends on it, never a handle.
static uint64_t key_hash(const uint64_t *w) {
    uint64_t x = 0x9e3779b97f4a7c15ULL;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < VK_WORDS; i++) {
        h = (h ^ w[i]) * x;
        h = (h >> 29) | (h << 35);
    }
    return h;
}

static void key_build(const struct VisualParams *p, const char *symbol) {
    // Interned so that a symbol pointer identifies its string: the key stores
    // the pointer, and equal keys must mean equal bytes.
    const char *interned = strtab_intern(symbol ? symbol : "");
    uint64_t *w = (uint64_t *)g_hdr;
    w[0] = (uint64_t)(uintptr_t)interned;
    uint8_t attrs = 0;
    attrs |= p->bold ? VF_BOLD : 0;
    attrs |= p->italic ? VF_ITALIC : 0;
    attrs |= p->underline ? VF_UNDERLINE : 0;
    attrs |= p->blink ? VF_BLINK : 0;
    attrs |= p->reverse ? VF_REVERSE : 0;
    attrs |= p->hidden ? VF_HIDDEN : 0;
    attrs |= p->strike ? VF_STRIKE : 0;
    w[1] = (uint64_t)attrs | ((uint64_t)(p->colors.has_fg ? 1u : 0u) << 8) |
           ((uint64_t)(p->colors.has_bg ? 1u : 0u) << 9) | ((uint64_t)(p->dim ? 1u : 0u) << 10);
    pack_code(&p->fg_code, p->has_fg_code, &w[2]);
    pack_code(&p->bg_code, p->has_bg_code, &w[4]);
}

// Writes one colour into the readable header in a form that matches color_eq.
static void header_put_color(char *at, const Color *c) {
    at[0] = c->is_xterm ? 1 : 0;
    at[1] = (char)c->xterm;
    uint8_t n = c->hex_len > sizeof(c->hex) ? (uint8_t)sizeof(c->hex) : c->hex_len;
    at[2] = (char)n;
    memset(at + 3, 0, sizeof(c->hex) + 3);
    memcpy(at + 3, c->hex, n);
    memcpy(at + 3 + sizeof(c->hex), c->rgb, 3);
}

static void logical_build(const struct VisualParams *p, const char *symbol) {
    const char *interned = strtab_intern(symbol ? symbol : "");  // stable, keyed by pointer
    memcpy(g_hdr + VH_SYMBOL, &interned, sizeof(interned));
    g_hdr[VH_DIM] = p->dim ? 1 : 0;
    uint8_t attrs = 0;
    attrs |= p->bold ? VF_BOLD : 0;
    attrs |= p->italic ? VF_ITALIC : 0;
    attrs |= p->underline ? VF_UNDERLINE : 0;
    attrs |= p->blink ? VF_BLINK : 0;
    attrs |= p->reverse ? VF_REVERSE : 0;
    attrs |= p->hidden ? VF_HIDDEN : 0;
    attrs |= p->strike ? VF_STRIKE : 0;
    g_hdr[VH_ATTRS] = (char)attrs;
    g_hdr[VH_FG] = p->colors.has_fg ? 1 : 0;
    header_put_color(g_hdr + VH_FG + 1, &p->colors.fg);
    g_hdr[VH_BG] = p->colors.has_bg ? 1 : 0;
    header_put_color(g_hdr + VH_BG + 1, &p->colors.bg);
}

static bool key_equal(const char *a, const char *b) {
    return memcmp(a, b, VK_BYTES) == 0;
}

static inline char *pool_header(uint32_t offset) {
    return (char *)g_visual_pool + offset - VH_SIZE;
}

// Formats the visual's bytes into g_fmt and returns the length. The sequence is
// the reference's: attributes, foreground code, background code, symbol, and a
// reset only if anything preceded the symbol.
static size_t format_bytes(const struct VisualParams *p, const char *symbol) {
    size_t n = 0;
#define PUSH(s)                                                                          \
    do {                                                                                 \
        size_t l = sizeof(s) - 1;                                                        \
        if (n + l + 1 > g_fmt_cap) {                                                     \
            g_fmt_cap = (n + l + 1) * 2;                                                 \
            g_fmt = realloc(g_fmt, g_fmt_cap);                                           \
            if (!g_fmt) {                                                                \
                visual_oom();                                                            \
            }                                                                            \
        }                                                                                \
        memcpy(g_fmt + n, s, l);                                                         \
        n += l;                                                                          \
    } while (0)
    if (p->bold) PUSH(ANSI_BOLD);
    if (p->italic) PUSH(ANSI_ITALIC);
    if (p->underline) PUSH(ANSI_UNDERLINE);
    if (p->blink) PUSH(ANSI_BLINK);
    if (p->reverse) PUSH(ANSI_REVERSE);
    if (p->hidden) PUSH(ANSI_HIDDEN);
    if (p->strike) PUSH(ANSI_STRIKETHROUGH);
#undef PUSH
    StrBuf sb = {g_fmt, n, g_fmt_cap};
    if (p->has_fg_code) {
        ansi_fg(&p->fg_code, &sb);
    }
    if (p->has_bg_code) {
        ansi_bg(&p->bg_code, &sb);
    }
    g_fmt = sb.data;
    g_fmt_cap = sb.cap;
    n = sb.len;
    size_t symbol_len = strlen(symbol);
    if (n + symbol_len + sizeof(ANSI_RESET_ALL) > g_fmt_cap) {
        g_fmt_cap = n + symbol_len + sizeof(ANSI_RESET_ALL) + 1;
        g_fmt = realloc(g_fmt, g_fmt_cap);
        if (!g_fmt) {
            visual_oom();
        }
    }
    memcpy(g_fmt + n, symbol, symbol_len);
    n += symbol_len;
    if (n != symbol_len) {
        memcpy(g_fmt + n, ANSI_RESET_ALL, sizeof(ANSI_RESET_ALL) - 1);
        n += sizeof(ANSI_RESET_ALL) - 1;
    }
    return n;
}

VisualHandle visual_make(const char *symbol, const struct VisualParams *p) {
    if (!g_ready) {
        visual_init();
    }
    if (strlen(symbol) > VISUAL_MAX) {
        return 0;
    }
    key_build(p, symbol);
    uint64_t hash = key_hash((const uint64_t *)g_hdr);
    uint32_t i = (uint32_t)hash & g_table_mask;
    for (;;) {
        VisEntry *e = &g_table[i];
        if (!e->handle) {
            break;
        }
        if (e->hash == hash && key_equal(pool_header(e->handle & VISUAL_OFFSET_MASK), g_hdr)) {
            return e->handle;
        }
        i = (i + 1) & g_table_mask;
    }
    size_t len = format_bytes(p, symbol);
    if (len > VISUAL_MAX) {
        len = VISUAL_MAX;
    }
    uint32_t base = g_pool_top;
    uint32_t need = (uint32_t)VH_SIZE + VISUAL_MAX + VISUAL_SLACK;
    if (base + need > POOL_LIMIT) {
        return 0;  // pool exhausted, faithfully
    }
    char *rec = (char *)g_visual_pool + base;
    char *bytes = rec + VH_SIZE;
    logical_build(p, symbol);          // only on a miss
    memcpy(rec, g_hdr, VH_SIZE);
    memcpy(bytes, g_fmt, len);
    memset(bytes + len, 0, VISUAL_SLACK);
    VisualHandle handle = (VisualHandle)(base + VH_SIZE) | ((VisualHandle)len << VISUAL_LEN_SHIFT);
    g_pool_top = base + need;
    g_table[i].handle = handle;
    g_table[i].hash = hash;
    g_table_used++;
    if ((g_table_used + 1) * 2 >= g_table_mask + 1) {
        // Grow and rehash by header.
        uint32_t cap = (g_table_mask + 1) * 2;
        VisEntry *grown = calloc(cap, sizeof(*grown));
        if (!grown) {
            visual_oom();
        }
        uint32_t mask = cap - 1;
        for (uint32_t k = 0; k <= g_table_mask; k++) {
            if (!g_table[k].handle) {
                continue;
            }
            uint32_t j = (uint32_t)g_table[k].hash & mask;
            while (grown[j].handle) {
                j = (j + 1) & mask;
            }
            grown[j] = g_table[k];
        }
        free(g_table);
        g_table = grown;
        g_table_mask = mask;
    }
    return handle;
}

const char *visual_symbol(VisualHandle h) {
    const char *interned;
    memcpy(&interned, pool_header(h & VISUAL_OFFSET_MASK) + VH_SYMBOL, sizeof(interned));
    return interned;
}

bool visual_colors(VisualHandle h, Color *fg, Color *bg, uint8_t *attrs) {
    const char *hdr = pool_header(h & VISUAL_OFFSET_MASK);
    memset(fg, 0, sizeof(*fg));
    memset(bg, 0, sizeof(*bg));
    bool has_fg = hdr[VH_FG] != 0;
    bool has_bg = hdr[VH_BG] != 0;
    Color *out[2] = {fg, bg};
    const char *at[2] = {hdr + VH_FG + 1, hdr + VH_BG + 1};
    bool has[2] = {has_fg, has_bg};
    for (int k = 0; k < 2; k++) {
        if (!has[k]) {
            continue;
        }
        const char *s = at[k];
        out[k]->is_xterm = s[0] != 0;
        out[k]->xterm = (uint8_t)s[1];
        uint8_t n = (uint8_t)s[2];
        if (n > sizeof(out[k]->hex)) {
            n = (uint8_t)sizeof(out[k]->hex);
        }
        out[k]->hex_len = n;
        memcpy(out[k]->hex, s + 3, n);
        memcpy(out[k]->rgb, s + 3 + sizeof(out[k]->hex), 3);
    }
    if (attrs) {
        *attrs = (uint8_t)hdr[VH_ATTRS];
    }
    return has_fg || has_bg;
}
