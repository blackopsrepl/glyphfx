#include <string.h>

#include "testutil.h"
#include "utils/ansi.h"

int main(void) {
    ColorCode c;
    CHECK_EQ_INT(ansi_parse_color_sequence("\x1b[38;2;255;0;128m", &c), 0);
    CHECK(c.kind == COLORCODE_RGB);
    CHECK(strcmp(c.hex, "FF0080") == 0);

    // Empty channel normalized to 0; prefix strip leaves two fields -> "0000".
    CHECK_EQ_INT(ansi_parse_color_sequence("\x1b[38;2;;0m", &c), 0);
    CHECK(strcmp(c.hex, "0000") == 0);

    CHECK_EQ_INT(ansi_parse_color_sequence("\x1b[48;5;42m", &c), 0);
    CHECK(c.kind == COLORCODE_XTERM);
    CHECK_EQ_INT(c.xterm, 42);

    CHECK_EQ_INT(ansi_parse_color_sequence("\x1b[1;2m", &c), -1);

    StrBuf sb;
    sb_init(&sb);
    ColorCode rgb = colorcode_rgb("ff0080");
    ansi_fg(&rgb, &sb);
    CHECK(strcmp(sb.data, "\x1b[38;2;255;0;128m") == 0);
    sb_clear(&sb);
    ColorCode xt = colorcode_xterm(42);
    ansi_bg(&xt, &sb);
    CHECK(strcmp(sb.data, "\x1b[48;5;42m") == 0);
    sb_clear(&sb);
    ansi_move_cursor_up(&sb, 3);
    CHECK(strcmp(sb.data, "\x1b[3A") == 0);
    sb_free(&sb);
    return test_summary("test_ansi");
}
