#include <string.h>

#include "testutil.h"
#include "utils/utf8.h"

static int decode_ok(const char *s) {
    uint32_t *cps = NULL;
    size_t n = 0;
    int rc = utf8_decode_strict(s, strlen(s), &cps, &n, NULL);
    free(cps);
    return rc == 0;
}

int main(void) {
    CHECK(decode_ok("hello"));
    CHECK(decode_ok("caf\xc3\xa9"));             // é
    CHECK(decode_ok("\xe4\xbd\xa0\xe5\xa5\xbd")); // 你好
    CHECK(decode_ok("\xf0\x9f\x98\x80"));         // emoji

    CHECK(!decode_ok("\xc0\x80"));         // overlong NUL
    CHECK(!decode_ok("\xed\xa0\x80"));     // surrogate U+D800
    CHECK(!decode_ok("\xf4\x90\x80\x80")); // > U+10FFFF
    CHECK(!decode_ok("\xe4\xbd"));         // truncated
    CHECK(!decode_ok("\xff"));             // invalid lead

    uint32_t *cps = NULL;
    size_t n = 0;
    CHECK_EQ_INT(utf8_decode_strict("ab", 2, &cps, &n, NULL), 0);
    CHECK_EQ_INT(n, 2);
    CHECK_EQ_INT(cps[0], 'a');
    CHECK_EQ_INT(cps[1], 'b');
    free(cps);
    return test_summary("test_utf8");
}
