// Gradient goldens from the reference fixture (tests/fixtures/graphics_goldens.txt).
#include <string.h>

#include "testutil.h"
#include "utils/graphics.h"

static void spectrum_to_string(const Gradient *g, char *out, size_t cap) {
    out[0] = '\0';
    size_t pos = 0;
    for (size_t i = 0; i < g->len; i++) {
        int n = snprintf(out + pos, cap - pos, "%s%s", i ? ";" : "", color_hex_text(&g->spectrum[i]));
        pos += (size_t)n;
    }
}

static int make_gradient(const char *const *stops, size_t n, const int64_t *steps, size_t n_steps, bool loop,
                         Gradient *out) {
    Color colors[8];
    for (size_t i = 0; i < n; i++) {
        if (color_from_hex(stops[i], &colors[i]) != 0) {
            return -1;
        }
    }
    return gradient_new(colors, n, steps, n_steps, false, loop, out);
}

int main(void) {
    char buf[2048];
    const char *stops1[] = {"8A008A", "00D1FF", "FFFFFF"};
    int64_t steps1[] = {12};
    Gradient g;
    CHECK_EQ_INT(make_gradient(stops1, 3, steps1, 1, false, &g), 0);
    spectrum_to_string(&g, buf, sizeof(buf));
    CHECK(strcmp(buf,
                 "8a008a;7e1193;72229c;6633a5;5a44ae;4e55b7;4266c0;3677c9;2a88d2;1e99db;12aae4;06bbed;00D1FF;"
                 "15d4ff;2ad7ff;3fdaff;54ddff;69e0ff;7ee3ff;93e6ff;a8e9ff;bdecff;d2efff;e7f2ff;FFFFFF") == 0);

    const char *f1 = color_hex_text(gradient_get_color_at_fraction(&g, 0.0));
    const char *f2 = color_hex_text(gradient_get_color_at_fraction(&g, 0.25));
    const char *f3 = color_hex_text(gradient_get_color_at_fraction(&g, 0.5));
    const char *f4 = color_hex_text(gradient_get_color_at_fraction(&g, 1.0));
    CHECK(strcmp(f1, "8a008a") == 0);
    CHECK(strcmp(f2, "4266c0") == 0);
    CHECK(strcmp(f3, "00D1FF") == 0);
    CHECK(strcmp(f4, "FFFFFF") == 0);
    CHECK(gradient_get_color_at_fraction(&g, 1.5) == NULL);
    gradient_free(&g);

    const char *stops2[] = {"ffffff", "000000"};
    int64_t steps2[] = {10};
    CHECK_EQ_INT(make_gradient(stops2, 2, steps2, 1, false, &g), 0);
    spectrum_to_string(&g, buf, sizeof(buf));
    CHECK(strcmp(buf, "ffffff;e5e5e5;cbcbcb;b1b1b1;979797;7d7d7d;636363;494949;2f2f2f;151515;000000") == 0);
    gradient_free(&g);

    const char *stops3[] = {"ff0000", "00ff00", "0000ff"};
    int64_t steps3[] = {5};
    CHECK_EQ_INT(make_gradient(stops3, 3, steps3, 1, true, &g), 0);
    spectrum_to_string(&g, buf, sizeof(buf));
    CHECK(strcmp(buf,
                 "ff0000;cc3300;996600;669900;33cc00;00ff00;00cc33;009966;006699;0033cc;0000ff;3300cc;660099;"
                 "990066;cc0033;ff0000") == 0);
    gradient_free(&g);

    const char *stops4[] = {"123456"};
    int64_t steps4[] = {4};
    CHECK_EQ_INT(make_gradient(stops4, 1, steps4, 1, false, &g), 0);
    spectrum_to_string(&g, buf, sizeof(buf));
    CHECK(strcmp(buf, "123456;123456;123456;123456") == 0);
    gradient_free(&g);

    // Coordinate mapping: vertical over (1..5, 1..8).
    const char *mstops[] = {"8A008A", "00D1FF", "FFFFFF"};
    int64_t msteps[] = {12};
    CHECK_EQ_INT(make_gradient(mstops, 3, msteps, 1, false, &g), 0);
    CoordColorMap map;
    CHECK_EQ_INT(gradient_build_coordinate_color_mapping(&g, 1, 5, 1, 8, GRADIENT_VERTICAL, &map), 0);
    CHECK(strcmp(color_hex_text(coordcolormap_get(&map, coord_new(1, 1))), "5a44ae") == 0);
    CHECK(strcmp(color_hex_text(coordcolormap_get(&map, coord_new(8, 1))), "5a44ae") == 0);
    CHECK(strcmp(color_hex_text(coordcolormap_get(&map, coord_new(1, 5))), "FFFFFF") == 0);
    CHECK(coordcolormap_get(&map, coord_new(9, 1)) == NULL);
    coordcolormap_free(&map);
    gradient_free(&g);

    return test_summary("test_gradient");
}
