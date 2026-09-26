// Easing functions, ported from the reference utils/easing.py. Python `x ** n`
// routes through C pow() even for integer exponents, so every power here is
// pow(), never repeated multiplication.
#ifndef GLYPHFX_EASING_H
#define GLYPHFX_EASING_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    EASE_LINEAR,
    EASE_IN_SINE,
    EASE_OUT_SINE,
    EASE_IN_OUT_SINE,
    EASE_IN_QUAD,
    EASE_OUT_QUAD,
    EASE_IN_OUT_QUAD,
    EASE_IN_CUBIC,
    EASE_OUT_CUBIC,
    EASE_IN_OUT_CUBIC,
    EASE_IN_QUART,
    EASE_OUT_QUART,
    EASE_IN_OUT_QUART,
    EASE_IN_QUINT,
    EASE_OUT_QUINT,
    EASE_IN_OUT_QUINT,
    EASE_IN_EXPO,
    EASE_OUT_EXPO,
    EASE_IN_OUT_EXPO,
    EASE_IN_CIRC,
    EASE_OUT_CIRC,
    EASE_IN_OUT_CIRC,
    EASE_IN_BACK,
    EASE_OUT_BACK,
    EASE_IN_OUT_BACK,
    EASE_IN_ELASTIC,
    EASE_OUT_ELASTIC,
    EASE_IN_OUT_ELASTIC,
    EASE_IN_BOUNCE,
    EASE_OUT_BOUNCE,
    EASE_IN_OUT_BOUNCE,
    EASE_CUBIC_BEZIER,
} EaseKind;

typedef struct Easing {
    EaseKind kind;
    double x1;
    double y1;
    double x2;
    double y2;
} Easing;

Easing easing_named(EaseKind kind);
Easing easing_cubic_bezier(double x1, double y1, double x2, double y2);
// Parses the 31 named functions; returns false on unknown names.
bool easing_parse(const char *s, Easing *out);
double easing_ease(const Easing *e, double p);

typedef struct {
    Easing easing_function;
    int64_t total_steps;
    bool clamp;
    int64_t current_step;
    double progress_ratio;
    double step_delta;
    double eased_value;
    double last_eased_value;
} EasingTracker;

void easing_tracker_init(EasingTracker *t, Easing easing_function, int64_t total_steps, bool clamp);
double easing_tracker_step(EasingTracker *t);
void easing_tracker_reset(EasingTracker *t);
bool easing_tracker_is_complete(const EasingTracker *t);

#endif
