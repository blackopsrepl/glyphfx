// Strict UTF-8 decoding matching Rust's String::from_utf8 / Python text mode:
// rejects overlong encodings, surrogates, and values above U+10FFFF.
#ifndef GLYPHFX_UTF8_H
#define GLYPHFX_UTF8_H

#include <stddef.h>
#include <stdint.h>

// Decodes bytes into a freshly allocated codepoint array. On success stores the
// pointer and count and returns 0; on invalid input returns -1 and sets
// *bad_offset to the byte offset of the first invalid sequence (if non-NULL).
int utf8_decode_strict(const char *bytes, size_t len, uint32_t **out_cps, size_t *out_count,
                       size_t *bad_offset);

// Counts UTF-8 codepoints (leading bytes only; assumes valid UTF-8).
size_t utf8_count_codepoints(const char *s);

#endif
