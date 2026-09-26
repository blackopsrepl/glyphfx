#include <string.h>

#include "testutil.h"
#include "utils/hexterm.h"

int main(void) {
    CHECK(strcmp(xterm_to_hex(0), "000000") == 0);
    CHECK(strcmp(xterm_to_hex(15), "ffffff") == 0);
    CHECK(strcmp(xterm_to_hex(196), "ff0000") == 0);

    CHECK_EQ_INT(hex_to_xterm("000000"), 0);
    CHECK_EQ_INT(hex_to_xterm("ffffff"), 15);
    // "ff0000" appears at both code 9 and 196; the first minimum (9) wins.
    CHECK_EQ_INT(hex_to_xterm("ff0000"), 9);
    // Case-insensitive, and seven-digit strings parse their first six digits.
    CHECK_EQ_INT(hex_to_xterm("#FF00AA"), hex_to_xterm("ff00aa"));
    CHECK_EQ_INT(hex_to_xterm("ff00aa7"), hex_to_xterm("ff00aa"));

    CHECK(is_valid_hex_color("#ff00aa"));
    CHECK(is_valid_hex_color("ff00aa"));
    CHECK(!is_valid_hex_color("ff00a"));
    CHECK(!is_valid_hex_color("gg00aa"));
    CHECK(is_valid_hex_color("1234567"));
    return test_summary("test_hexterm");
}
