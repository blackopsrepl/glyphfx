#include "utils/geometry.h"

#include <math.h>
#include <stdlib.h>

#include "utils/pycompat.h"

void coordvec_init(CoordVec *v) {
    v->items = NULL;
    v->len = 0;
    v->cap = 0;
}

void coordvec_push(CoordVec *v, Coord c) {
    if (v->len == v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 16;
        Coord *grown = realloc(v->items, cap * sizeof(Coord));
        if (!grown) {
            return;
        }
        v->items = grown;
        v->cap = cap;
    }
    v->items[v->len++] = c;
}

void coordvec_free(CoordVec *v) {
    free(v->items);
    coordvec_init(v);
}

typedef struct {
    double column;
    double row;
} FloatPoint;

static FloatPoint fp_interp(FloatPoint a, FloatPoint b, double t) {
    FloatPoint p;
    p.column = (1.0 - t) * a.column + t * b.column;
    p.row = (1.0 - t) * a.row + t * b.row;
    return p;
}

CoordVec find_coords_on_circle(Coord origin, int64_t radius, int64_t coords_limit, bool unique) {
    CoordVec points;
    coordvec_init(&points);
    if (radius == 0) {
        return points;
    }
    if (coords_limit == 0) {
        coords_limit = py_round_half_even(2.0 * M_PI * (double)radius);
    }
    double angle_step = 2.0 * M_PI / (double)coords_limit;
    for (int64_t i = 0; i < coords_limit; i++) {
        double angle = angle_step * (double)i;
        double x = (double)origin.column + (double)radius * cos(angle);
        double x_diff = x - (double)origin.column;
        x += x_diff;
        double y = (double)origin.row + (double)radius * sin(angle);
        Coord point = coord_new(py_round_half_even(x), py_round_half_even(y));
        if (unique) {
            bool seen = false;
            for (size_t k = 0; k < points.len; k++) {
                if (coord_eq(points.items[k], point)) {
                    seen = true;
                    break;
                }
            }
            if (!seen) {
                coordvec_push(&points, point);
            }
        } else {
            coordvec_push(&points, point);
        }
    }
    return points;
}

static int64_t circle_max_y_offset(int64_t x, int64_t h, int64_t k, double a_squared, double b_squared, int64_t *out) {
    (void)k;
    double x_component = pow((double)(x - h), 2.0) / a_squared;
    *out = (int64_t)pow(b_squared * (1.0 - x_component), 0.5);
    return 0;
}

CoordVec find_coords_in_circle(Coord center, int64_t diameter) {
    CoordVec coords;
    coordvec_init(&coords);
    if (diameter == 0) {
        return coords;
    }
    int64_t h = center.column;
    int64_t k = center.row;
    double a_squared = pow((double)diameter, 2.0);
    double b_squared = pow((double)diameter / 2.0, 2.0);
    for (int64_t x = h - diameter; x <= h + diameter; x++) {
        int64_t max_offset = 0;
        circle_max_y_offset(x, h, k, a_squared, b_squared, &max_offset);
        for (int64_t y = k - max_offset; y <= k + max_offset; y++) {
            coordvec_push(&coords, coord_new(x, y));
        }
    }
    return coords;
}

CoordVec find_coords_in_rect(Coord origin, int64_t distance) {
    CoordVec coords;
    coordvec_init(&coords);
    if (distance == 0) {
        return coords;
    }
    for (int64_t column = origin.column - distance; column <= origin.column + distance; column++) {
        for (int64_t row = origin.row - distance; row <= origin.row + distance; row++) {
            coordvec_push(&coords, coord_new(column, row));
        }
    }
    return coords;
}

CoordVec find_coords_on_rect(Coord origin, int64_t half_width, int64_t half_height) {
    CoordVec coords;
    coordvec_init(&coords);
    if (half_width == 0 || half_height == 0) {
        return coords;
    }
    for (int64_t column = origin.column - half_width; column <= origin.column + half_width; column++) {
        if (column == origin.column - half_width || column == origin.column + half_width) {
            for (int64_t row = origin.row - half_height; row <= origin.row + half_height; row++) {
                coordvec_push(&coords, coord_new(column, row));
            }
        } else {
            coordvec_push(&coords, coord_new(column, origin.row - half_height));
            coordvec_push(&coords, coord_new(column, origin.row + half_height));
        }
    }
    return coords;
}

double find_length_of_line(Coord coord1, Coord coord2, bool double_row_diff) {
    double column_diff = (double)(coord2.column - coord1.column);
    double row_diff = (double)(coord2.row - coord1.row);
    if (double_row_diff) {
        return hypot(column_diff, 2.0 * row_diff);
    }
    return hypot(column_diff, row_diff);
}

Coord extrapolate_along_ray(Coord origin, Coord target, double offset_from_target) {
    double base = find_length_of_line(origin, target, false);
    double total_distance = base + offset_from_target;
    if (total_distance == 0.0 || coord_eq(origin, target)) {
        return target;
    }
    double t = total_distance / base;
    double next_column = (1.0 - t) * (double)origin.column + t * (double)target.column;
    double next_row = (1.0 - t) * (double)origin.row + t * (double)target.row;
    return coord_new(py_round_half_even(next_column), py_round_half_even(next_row));
}

Coord find_coord_on_line(Coord start, Coord end, double t) {
    double x = (1.0 - t) * (double)start.column + t * (double)end.column;
    double y = (1.0 - t) * (double)start.row + t * (double)end.row;
    return coord_new(py_round_half_even(x), py_round_half_even(y));
}

Coord find_coord_on_bezier_curve(Coord start, const Coord *control, size_t control_len, Coord end, double t) {
    if (control_len == 0) {
        return find_coord_on_line(start, end, t);
    }
    FloatPoint s = {(double)start.column, (double)start.row};
    FloatPoint e = {(double)end.column, (double)end.row};
    if (control_len == 1) {
        FloatPoint c = {(double)control[0].column, (double)control[0].row};
        FloatPoint p = fp_interp(fp_interp(s, c, t), fp_interp(c, e, t), t);
        return coord_new(py_round_half_even(p.column), py_round_half_even(p.row));
    }
    size_t n = control_len + 2;
    FloatPoint *points = malloc(n * sizeof(FloatPoint));
    if (!points) {
        return start;
    }
    points[0] = s;
    for (size_t i = 0; i < control_len; i++) {
        points[i + 1].column = (double)control[i].column;
        points[i + 1].row = (double)control[i].row;
    }
    points[n - 1] = e;
    size_t remaining = n;
    while (remaining > 1) {
        for (size_t i = 0; i + 1 < remaining; i++) {
            points[i] = fp_interp(points[i], points[i + 1], t);
        }
        remaining -= 1;
    }
    Coord result = coord_new(py_round_half_even(points[0].column), py_round_half_even(points[0].row));
    free(points);
    return result;
}

double find_length_of_bezier_curve(Coord start, const Coord *control, size_t control_len, Coord end) {
    double length = 0.0;
    Coord prev = start;
    for (int t = 1; t < 10; t++) {
        Coord c = find_coord_on_bezier_curve(start, control, control_len, end, (double)t / 10.0);
        length += find_length_of_line(prev, c, true);
        prev = c;
    }
    return length;
}

int find_normalized_distance_from_center(int64_t bottom, int64_t top, int64_t left, int64_t right, Coord other,
                                         double *out) {
    int64_t y_offset = bottom - 1;
    int64_t x_offset = left - 1;
    right = right - x_offset;
    top = top - y_offset;
    double center_x = (double)right / 2.0;
    double center_y = (double)top / 2.0;

    int64_t col = other.column - x_offset;
    int64_t row = other.row - y_offset;
    if (!(col >= left - x_offset && col <= right) || !(row >= bottom - y_offset && row <= top)) {
        return -1;
    }

    double max_distance = pow(pow((double)right, 2.0) + pow((double)(top * 2), 2.0), 0.5);
    double distance = pow(pow((double)col - center_x, 2.0) + pow(((double)row - center_y) * 2.0, 2.0), 0.5);
    *out = distance / (max_distance / 2.0);
    return 0;
}
