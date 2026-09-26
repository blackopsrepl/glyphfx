// ANSI escape sequences and color codes, ported from the reference
// utils/ansitools.py + utils/colorterm.py.
#ifndef GLYPHFX_ANSI_H
#define GLYPHFX_ANSI_H

#include <stdint.h>

#include "utils/strbuf.h"

#define ANSI_DEC_SAVE_CURSOR "\x1b" "7"
#define ANSI_DEC_RESTORE_CURSOR "\x1b" "8"
#define ANSI_HIDE_CURSOR "\x1b[?25l"
#define ANSI_SHOW_CURSOR "\x1b[?25h"
#define ANSI_RESET_ALL "\x1b[0m"
#define ANSI_CLEAR_TO_END_OF_SCREEN "\x1b[0J"
#define ANSI_BOLD "\x1b[1m"
#define ANSI_DIM "\x1b[2m"
#define ANSI_ITALIC "\x1b[3m"
#define ANSI_UNDERLINE "\x1b[4m"
#define ANSI_BLINK "\x1b[5m"
#define ANSI_REVERSE "\x1b[7m"
#define ANSI_HIDDEN "\x1b[8m"
#define ANSI_STRIKETHROUGH "\x1b[9m"

typedef enum {
    COLORCODE_RGB,
    COLORCODE_XTERM,
} ColorCodeKind;

// A resolved color ready for SGR emission. The RGB form carries the hex text
// without '#'; its case is preserved as the reference passes it through.
typedef struct {
    ColorCodeKind kind;
    char hex[16];
    uint8_t xterm;
} ColorCode;

ColorCode colorcode_rgb(const char *hex);
ColorCode colorcode_xterm(uint8_t code);

void ansi_fg(const ColorCode *code, StrBuf *out);
void ansi_bg(const ColorCode *code, StrBuf *out);

void ansi_move_cursor_up(StrBuf *out, int64_t y);

// Parses e.g. "\x1b[38;2;255;0;128m". Returns 0 on success.
int ansi_parse_color_sequence(const char *sequence, ColorCode *out);

#endif
