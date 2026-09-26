// Coordinate and geometry math, ported from the reference utils/geometry.py.
// All rounding is banker's rounding (pycompat); int() casts truncate.
#ifndef GLYPHFX_GEOMETRY_H
#define GLYPHFX_GEOMETRY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct {
    int64_t column;
    int64_t row;
} Coord;

static inline Coord coord_new(int64_t column, int64_t row) {
    Coord c = {column, row};
    return c;
}

static inline bool coord_eq(Coord a, Coord b) {
    return a.column == b.column && a.row == b.row;
}

typedef struct {
    Coord *items;
    size_t len;
    size_t cap;
} CoordVec;

void coordvec_init(CoordVec *v);
void coordvec_push(CoordVec *v, Coord c);
void coordvec_free(CoordVec *v);

CoordVec find_coords_on_circle(Coord origin, int64_t radius, int64_t coords_limit, bool unique);
CoordVec find_coords_in_circle(Coord center, int64_t diameter);
CoordVec find_coords_in_rect(Coord origin, int64_t distance);
CoordVec find_coords_on_rect(Coord origin, int64_t half_width, int64_t half_height);
Coord extrapolate_along_ray(Coord origin, Coord target, double offset_from_target);
Coord find_coord_on_bezier_curve(Coord start, const Coord *control, size_t control_len, Coord end, double t);
Coord find_coord_on_line(Coord start, Coord end, double t);
double find_length_of_bezier_curve(Coord start, const Coord *control, size_t control_len, Coord end);
double find_length_of_line(Coord coord1, Coord coord2, bool double_row_diff);
// Returns 0 and sets *out on success; -1 when other_coord is outside the rect.
int find_normalized_distance_from_center(int64_t bottom, int64_t top, int64_t left, int64_t right, Coord other,
                                         double *out);

#endif
