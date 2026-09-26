// Coordinate and geometry math, ported from the reference utils/geometry.py.
#ifndef GLYPHFX_GEOMETRY_H
#define GLYPHFX_GEOMETRY_H

#include <stdbool.h>
#include <stdint.h>

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

#endif
