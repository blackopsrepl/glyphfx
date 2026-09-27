// The visual pool's byte output must be exactly what the old vis_format
// produced, and its handles must intern by appearance.
#include <string.h>

#include "engine/animation.h"
#include "engine/visual.h"
#include "testutil.h"

static void params_plain(VisualParams *p) {
    memset(p, 0, sizeof(*p));
}

static void params_attr(VisualParams *p, bool bold, bool italic) {
    params_plain(p);
    p->bold = bold;
    p->italic = italic;
}

static void params_fg_xterm(VisualParams *p, uint8_t code) {
    params_plain(p);
    p->has_colors = true;
    p->colors.has_fg = true;
    p->colors.fg = color_from_xterm(code);
    p->has_fg_code = true;
    p->fg_code = colorcode_xterm(code);
}

static void params_fg_hex(VisualParams *p, const char *hex) {
    params_plain(p);
    p->has_colors = true;
    p->colors.has_fg = true;
    color_from_hex(hex, &p->colors.fg);
    p->has_fg_code = true;
    p->fg_code = colorcode_rgb(hex);
}

int main(void) {
    visual_init();

    VisualParams p;
    params_plain(&p);
    VisualHandle sym = visual_make("X", &p);
    CHECK(sym != 0);
    CHECK_EQ_INT(visual_len(sym), 1);
    CHECK(memcmp(visual_bytes(sym), "X", 1) == 0);
    // The plain visual emits no reset: nothing preceded the symbol.
    CHECK_EQ_INT(visual_len(sym), 1);

    // A restyle to the same appearance interns to the same handle.
    params_plain(&p);
    CHECK_EQ_INT(visual_make("X", &p), sym);

    // Bold emits the attribute and a reset after the symbol.
    params_attr(&p, true, false);
    VisualHandle bold = visual_make("X", &p);
    CHECK(bold != sym);
    CHECK(memcmp(visual_bytes(bold), "\x1b[1mX\x1b[0m", 9) == 0);
    CHECK_EQ_INT(visual_len(bold), 9);

    // Bold and italic stack in VisualParams order.
    params_attr(&p, true, true);
    VisualHandle bi = visual_make("X", &p);
    CHECK(memcmp(visual_bytes(bi), "\x1b[1m\x1b[3mX\x1b[0m", 12) == 0);

    // An xterm foreground renders as its 256-colour code.
    params_fg_xterm(&p, 1);
    VisualHandle xt = visual_make("X", &p);
    CHECK_EQ_INT(visual_len(xt), (uint32_t)strlen("\x1b[38;5;1mX\x1b[0m"));
    CHECK(memcmp(visual_bytes(xt), "\x1b[38;5;1mX\x1b[0m", visual_len(xt)) == 0);

    // A 24-bit foreground renders its RGB.
    params_fg_hex(&p, "ff8800");
    VisualHandle rgb = visual_make("X", &p);
    CHECK(memcmp(visual_bytes(rgb), "\x1b[38;2;255;136;0mX\x1b[0m", visual_len(rgb)) == 0);

    // The header reads back what the visual is.
    CHECK(strcmp(visual_symbol(rgb), "X") == 0);
    Color fg, bg;
    uint8_t attrs = 0;
    CHECK(!visual_colors(bold, &fg, &bg, &attrs));  // bold carries no colour
    CHECK((attrs & VF_BOLD) != 0);
    CHECK(!visual_colors(sym, &fg, &bg, &attrs));  // plain X carries none
    CHECK_EQ_INT(attrs, 0);
    // A coloured visual reads back its logical colour.
    CHECK(visual_colors(rgb, &fg, &bg, &attrs));
    CHECK_EQ_INT(fg.rgb[0], 255);
    CHECK_EQ_INT(fg.rgb[1], 136);
    CHECK_EQ_INT(fg.rgb[2], 0);

    // Distinct symbols never share a handle.
    params_plain(&p);
    CHECK(visual_make("Y", &p) != sym);

    // Every visual is readable for VISUAL_SLACK bytes past its start.
    volatile unsigned char sink = 0;
    const char *bytes = visual_bytes(rgb);
    for (size_t i = 0; i < VISUAL_SLACK; i++) {
        sink = (unsigned char)(sink ^ bytes[i]);
    }
    CHECK(sink == sink);

    // The handle packs a pool offset and the length.
    CHECK((rgb & VISUAL_OFFSET_MASK) != 0);
    CHECK_EQ_INT(rgb & VISUAL_OFFSET_MASK & 15u, (unsigned)VISUAL_HEADER & 15u);

    // A symbol longer than the pool's maximum is refused, not truncated.
    char big[VISUAL_MAX + 2];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    params_plain(&p);
    CHECK_EQ_INT(visual_make(big, &p), 0);

    visual_free();
    return test_summary("test_visual");
}
