// Geometry goldens from the reference fixture (tests/fixtures/geometry_goldens.txt).
#include <string.h>

#include "testutil.h"
#include "utils/geometry.h"

static uint64_t bits(double v) {
    uint64_t b;
    memcpy(&b, &v, sizeof(b));
    return b;
}

static void coords_to_string(CoordVec *v, char *out, size_t cap) {
    out[0] = '\0';
    size_t pos = 0;
    for (size_t i = 0; i < v->len; i++) {
        int n = snprintf(out + pos, cap - pos, "%s%lld,%lld", i ? ";" : "", (long long)v->items[i].column,
                         (long long)v->items[i].row);
        pos += (size_t)n;
    }
}

int main(void) {
    CoordVec v = find_coords_in_rect(coord_new(3, 4), 1);
    char buf[512];
    coords_to_string(&v, buf, sizeof(buf));
    CHECK(strcmp(buf, "2,3;2,4;2,5;3,3;3,4;3,5;4,3;4,4;4,5") == 0);
    CHECK_EQ_INT(v.len, 9);
    coordvec_free(&v);

    v = find_coords_on_rect(coord_new(0, 0), 1, 1);
    coords_to_string(&v, buf, sizeof(buf));
    CHECK(strcmp(buf, "-1,-1;-1,0;-1,1;0,-1;0,1;1,-1;1,0;1,1") == 0);
    coordvec_free(&v);

    v = find_coords_in_circle(coord_new(5, -3), 1);
    coords_to_string(&v, buf, sizeof(buf));
    CHECK(strcmp(buf, "4,-3;5,-3;6,-3") == 0);
    coordvec_free(&v);

    CHECK(coord_eq(find_coord_on_line(coord_new(-3, 7), coord_new(14, -2), -5.0 / 20.0), coord_new(-7, 9)));
    CHECK(coord_eq(find_coord_on_line(coord_new(-3, 7), coord_new(14, -2), 25.0 / 20.0), coord_new(18, -4)));

    CHECK(bits(find_length_of_line(coord_new(1, 2), coord_new(-7, 11), false)) == 0x4028154be2773526ULL);
    CHECK(bits(find_length_of_line(coord_new(1, 2), coord_new(-7, 11), true)) == 0x4033b29d7d635662ULL);

    double nd = 0.0;
    CHECK_EQ_INT(find_normalized_distance_from_center(1, 8, 1, 10, coord_new(1, 1), &nd), 0);
    CHECK(bits(nd) == 0x3fe875c346c811eeULL);
    CHECK_EQ_INT(find_normalized_distance_from_center(1, 8, 1, 10, coord_new(10, 8), &nd), 0);
    CHECK(bits(nd) == 0x3ff0000000000000ULL);
    // Out-of-rectangle coords are rejected (reference ValueError).
    CHECK_EQ_INT(find_normalized_distance_from_center(1, 8, 1, 10, coord_new(0, 0), &nd), -1);

    return test_summary("test_geometry");
}
