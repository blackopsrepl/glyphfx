// Minimal growable byte buffer. Always NUL-terminated so it can be read as a
// C string; .len excludes the terminator. Embedded NULs are preserved in the
// byte range but callers reading it as C string will stop early.
#ifndef GLYPHFX_STRBUF_H
#define GLYPHFX_STRBUF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
void sb_free(StrBuf *sb);
void sb_clear(StrBuf *sb);
void sb_reserve(StrBuf *sb, size_t extra);
void sb_push(StrBuf *sb, char c);
void sb_append(StrBuf *sb, const char *bytes, size_t len);
void sb_puts(StrBuf *sb, const char *s);
void sb_printf(StrBuf *sb, const char *fmt, ...);
void sb_append_byte(StrBuf *sb, uint8_t b);
// Appends the codepoint encoded as UTF-8.
void sb_append_cp(StrBuf *sb, uint32_t cp);
// Takes ownership of the buffer contents; resets sb. Caller frees.
char *sb_take(StrBuf *sb);

#endif
