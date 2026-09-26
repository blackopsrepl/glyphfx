#include "utils/strbuf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sb_oom(void) {
    fputs("glyphfx: out of memory\n", stderr);
    abort();
}

void sb_init(StrBuf *sb) {
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void sb_free(StrBuf *sb) {
    free(sb->data);
    sb_init(sb);
}

void sb_grow(StrBuf *sb, size_t need) {
    size_t required = need + 1;
    if (required <= sb->cap) {
        return;
    }
    size_t cap = sb->cap ? sb->cap : 32;
    while (cap < required) {
        cap *= 2;
    }
    char *grown = realloc(sb->data, cap);
    if (!grown) {
        sb_oom();
    }
    sb->data = grown;
    sb->cap = cap;
}

void sb_reserve(StrBuf *sb, size_t extra) {
    sb_grow(sb, sb->len + extra);
}

void sb_puts(StrBuf *sb, const char *s) {
    sb_append(sb, s, strlen(s));
}

void sb_printf(StrBuf *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int needed = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (needed < 0) {
        va_end(args);
        return;
    }
    sb_reserve(sb, (size_t)needed);
    (void)vsnprintf(sb->data + sb->len, (size_t)needed + 1, fmt, args);
    sb->len += (size_t)needed;
    va_end(args);
}

void sb_append_byte(StrBuf *sb, uint8_t b) {
    sb_append(sb, (const char *)&b, 1);
}

void sb_append_cp(StrBuf *sb, uint32_t cp) {
    if (cp < 0x80) {
        sb_push(sb, (char)cp);
    } else if (cp < 0x800) {
        sb_push(sb, (char)(0xC0 | (cp >> 6)));
        sb_push(sb, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        sb_push(sb, (char)(0xE0 | (cp >> 12)));
        sb_push(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_push(sb, (char)(0x80 | (cp & 0x3F)));
    } else {
        sb_push(sb, (char)(0xF0 | (cp >> 18)));
        sb_push(sb, (char)(0x80 | ((cp >> 12) & 0x3F)));
        sb_push(sb, (char)(0x80 | ((cp >> 6) & 0x3F)));
        sb_push(sb, (char)(0x80 | (cp & 0x3F)));
    }
}

char *sb_take(StrBuf *sb) {
    char *out = sb->data;
    if (!out) {
        out = malloc(1);
        if (!out) {
            sb_oom();
        }
        out[0] = '\0';
    }
    sb_init(sb);
    return out;
}
