#include "utils/pycompat.h"

#include <math.h>

// For finite values, nearbyint under the default FE_TONEAREST rounding mode is
// exactly round-half-to-even, matching Rust's f64::round_ties_even. Non-finite
// values follow the reference's saturating cast behavior.
int64_t py_round_half_even(double x) {
    if (isfinite(x)) {
        return (int64_t)nearbyint(x);
    }
    if (isnan(x)) {
        return 0;
    }
    return x > 0 ? INT64_MAX : INT64_MIN;
}

int64_t py_floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if (a % b != 0 && (a < 0) != (b < 0)) {
        return q - 1;
    }
    return q;
}

int64_t py_mod(int64_t a, int64_t b) {
    int64_t r = a % b;
    if (r != 0 && (r < 0) != (b < 0)) {
        return r + b;
    }
    return r;
}
