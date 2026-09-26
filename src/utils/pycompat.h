// Python-compatible numeric semantics used throughout the engine.
// Every call site that transcribes a Python round() or // must route through
// here; see reference plan.md section 5.
#ifndef GLYPHFX_PYCOMPAT_H
#define GLYPHFX_PYCOMPAT_H

#include <stdint.h>

// Python round(): banker's rounding (half to even), result as int64.
int64_t py_round_half_even(double x);

// Python // on integers: floor division.
int64_t py_floor_div(int64_t a, int64_t b);

// Python % on integers: result takes the sign of the divisor.
int64_t py_mod(int64_t a, int64_t b);

#endif
