#include "utils/easing.h"

#include <math.h>
#include <string.h>

#define EASE_PI 3.14159265358979323846

Easing easing_named(EaseKind kind) {
    Easing e;
    memset(&e, 0, sizeof(e));
    e.kind = kind;
    return e;
}

Easing easing_cubic_bezier(double x1, double y1, double x2, double y2) {
    Easing e;
    e.kind = EASE_CUBIC_BEZIER;
    e.x1 = x1;
    e.y1 = y1;
    e.x2 = x2;
    e.y2 = y2;
    return e;
}

bool easing_parse(const char *s, Easing *out) {
    // case-insensitive match against the reference names
    char buf[32];
    size_t n = strlen(s);
    if (n >= sizeof(buf)) {
        return false;
    }
    for (size_t i = 0; i <= n; i++) {
        char c = s[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    if (strcmp(buf, "linear") == 0) *out = easing_named(EASE_LINEAR);
    else if (strcmp(buf, "in_sine") == 0) *out = easing_named(EASE_IN_SINE);
    else if (strcmp(buf, "out_sine") == 0) *out = easing_named(EASE_OUT_SINE);
    else if (strcmp(buf, "in_out_sine") == 0) *out = easing_named(EASE_IN_OUT_SINE);
    else if (strcmp(buf, "in_quad") == 0) *out = easing_named(EASE_IN_QUAD);
    else if (strcmp(buf, "out_quad") == 0) *out = easing_named(EASE_OUT_QUAD);
    else if (strcmp(buf, "in_out_quad") == 0) *out = easing_named(EASE_IN_OUT_QUAD);
    else if (strcmp(buf, "in_cubic") == 0) *out = easing_named(EASE_IN_CUBIC);
    else if (strcmp(buf, "out_cubic") == 0) *out = easing_named(EASE_OUT_CUBIC);
    else if (strcmp(buf, "in_out_cubic") == 0) *out = easing_named(EASE_IN_OUT_CUBIC);
    else if (strcmp(buf, "in_quart") == 0) *out = easing_named(EASE_IN_QUART);
    else if (strcmp(buf, "out_quart") == 0) *out = easing_named(EASE_OUT_QUART);
    else if (strcmp(buf, "in_out_quart") == 0) *out = easing_named(EASE_IN_OUT_QUART);
    else if (strcmp(buf, "in_quint") == 0) *out = easing_named(EASE_IN_QUINT);
    else if (strcmp(buf, "out_quint") == 0) *out = easing_named(EASE_OUT_QUINT);
    else if (strcmp(buf, "in_out_quint") == 0) *out = easing_named(EASE_IN_OUT_QUINT);
    else if (strcmp(buf, "in_expo") == 0) *out = easing_named(EASE_IN_EXPO);
    else if (strcmp(buf, "out_expo") == 0) *out = easing_named(EASE_OUT_EXPO);
    else if (strcmp(buf, "in_out_expo") == 0) *out = easing_named(EASE_IN_OUT_EXPO);
    else if (strcmp(buf, "in_circ") == 0) *out = easing_named(EASE_IN_CIRC);
    else if (strcmp(buf, "out_circ") == 0) *out = easing_named(EASE_OUT_CIRC);
    else if (strcmp(buf, "in_out_circ") == 0) *out = easing_named(EASE_IN_OUT_CIRC);
    else if (strcmp(buf, "in_back") == 0) *out = easing_named(EASE_IN_BACK);
    else if (strcmp(buf, "out_back") == 0) *out = easing_named(EASE_OUT_BACK);
    else if (strcmp(buf, "in_out_back") == 0) *out = easing_named(EASE_IN_OUT_BACK);
    else if (strcmp(buf, "in_elastic") == 0) *out = easing_named(EASE_IN_ELASTIC);
    else if (strcmp(buf, "out_elastic") == 0) *out = easing_named(EASE_OUT_ELASTIC);
    else if (strcmp(buf, "in_out_elastic") == 0) *out = easing_named(EASE_IN_OUT_ELASTIC);
    else if (strcmp(buf, "in_bounce") == 0) *out = easing_named(EASE_IN_BOUNCE);
    else if (strcmp(buf, "out_bounce") == 0) *out = easing_named(EASE_OUT_BOUNCE);
    else if (strcmp(buf, "in_out_bounce") == 0) *out = easing_named(EASE_IN_OUT_BOUNCE);
    else return false;
    return true;
}

static double out_bounce(double p) {
    double n1 = 7.5625;
    double d1 = 2.75;
    if (p < 1.0 / d1) {
        return n1 * pow(p, 2.0);
    }
    if (p < 2.0 / d1) {
        return n1 * pow(p - 1.5 / d1, 2.0) + 0.75;
    }
    if (p < 2.5 / d1) {
        return n1 * pow(p - 2.25 / d1, 2.0) + 0.9375;
    }
    return n1 * pow(p - 2.625 / d1, 2.0) + 0.984375;
}

static double bezier_easing(double x1, double y1, double x2, double y2, double progress) {
    if (progress <= 0.0) {
        return 0.0;
    }
    if (progress >= 1.0) {
        return 1.0;
    }
    double t = progress;
    for (int i = 0; i < 20; i++) {
        double omt = 1.0 - t;
        double x_est = 3.0 * x1 * pow(omt, 2.0) * t + 3.0 * x2 * omt * pow(t, 2.0) + pow(t, 3.0);
        double dx = x_est - progress;
        if (fabs(dx) < 1e-5) {
            break;
        }
        double d = 3.0 * pow(1.0 - t, 2.0) * x1 + 6.0 * (1.0 - t) * t * (x2 - x1) + 3.0 * pow(t, 2.0) * (1.0 - x2);
        if (fabs(d) < 1e-6) {
            break;
        }
        t -= dx / d;
    }
    double omt = 1.0 - t;
    return 3.0 * y1 * pow(omt, 2.0) * t + 3.0 * y2 * omt * pow(t, 2.0) + pow(t, 3.0);
}

double easing_ease(const Easing *e, double p) {
    switch (e->kind) {
        case EASE_LINEAR:
            return p;
        case EASE_IN_SINE:
            return 1.0 - cos((p * EASE_PI) / 2.0);
        case EASE_OUT_SINE:
            return sin((p * EASE_PI) / 2.0);
        case EASE_IN_OUT_SINE:
            return -(cos(EASE_PI * p) - 1.0) / 2.0;
        case EASE_IN_QUAD:
            return pow(p, 2.0);
        case EASE_OUT_QUAD:
            return 1.0 - (1.0 - p) * (1.0 - p);
        case EASE_IN_OUT_QUAD:
            return p < 0.5 ? 2.0 * pow(p, 2.0) : 1.0 - pow(-2.0 * p + 2.0, 2.0) / 2.0;
        case EASE_IN_CUBIC:
            return pow(p, 3.0);
        case EASE_OUT_CUBIC:
            return 1.0 - pow(1.0 - p, 3.0);
        case EASE_IN_OUT_CUBIC:
            return p < 0.5 ? 4.0 * pow(p, 3.0) : 1.0 - pow(-2.0 * p + 2.0, 3.0) / 2.0;
        case EASE_IN_QUART:
            return pow(p, 4.0);
        case EASE_OUT_QUART:
            return 1.0 - pow(1.0 - p, 4.0);
        case EASE_IN_OUT_QUART:
            return p < 0.5 ? 8.0 * pow(p, 4.0) : 1.0 - pow(-2.0 * p + 2.0, 4.0) / 2.0;
        case EASE_IN_QUINT:
            return pow(p, 5.0);
        case EASE_OUT_QUINT:
            return 1.0 - pow(1.0 - p, 5.0);
        case EASE_IN_OUT_QUINT:
            return p < 0.5 ? 16.0 * pow(p, 5.0) : 1.0 - pow(-2.0 * p + 2.0, 5.0) / 2.0;
        case EASE_IN_EXPO:
            return p == 0.0 ? 0.0 : pow(2.0, 10.0 * p - 10.0);
        case EASE_OUT_EXPO:
            return p == 1.0 ? 1.0 : 1.0 - pow(2.0, -10.0 * p);
        case EASE_IN_OUT_EXPO:
            if (p == 0.0) return 0.0;
            if (p == 1.0) return 1.0;
            if (p < 0.5) return pow(2.0, 20.0 * p - 10.0) / 2.0;
            return (2.0 - pow(2.0, -20.0 * p + 10.0)) / 2.0;
        case EASE_IN_CIRC:
            return 1.0 - sqrt(1.0 - pow(p, 2.0));
        case EASE_OUT_CIRC:
            return sqrt(1.0 - pow(p - 1.0, 2.0));
        case EASE_IN_OUT_CIRC:
            if (p < 0.5) return (1.0 - sqrt(1.0 - pow(2.0 * p, 2.0))) / 2.0;
            return (sqrt(1.0 - pow(-2.0 * p + 2.0, 2.0)) + 1.0) / 2.0;
        case EASE_IN_BACK: {
            double c1 = 1.70158;
            double c3 = c1 + 1.0;
            return c3 * pow(p, 3.0) - c1 * pow(p, 2.0);
        }
        case EASE_OUT_BACK: {
            double c1 = 1.70158;
            double c3 = c1 + 1.0;
            return 1.0 + c3 * pow(p - 1.0, 3.0) + c1 * pow(p - 1.0, 2.0);
        }
        case EASE_IN_OUT_BACK: {
            double c1 = 1.70158;
            double c2 = c1 * 1.525;
            if (p < 0.5) {
                return (pow(2.0 * p, 2.0) * ((c2 + 1.0) * 2.0 * p - c2)) / 2.0;
            }
            return (pow(2.0 * p - 2.0, 2.0) * ((c2 + 1.0) * (p * 2.0 - 2.0) + c2) + 2.0) / 2.0;
        }
        case EASE_IN_ELASTIC: {
            double c4 = (2.0 * EASE_PI) / 3.0;
            if (p == 0.0) return 0.0;
            if (p == 1.0) return 1.0;
            return -pow(2.0, 10.0 * p - 10.0) * sin((p * 10.0 - 10.75) * c4);
        }
        case EASE_OUT_ELASTIC: {
            double c4 = (2.0 * EASE_PI) / 3.0;
            if (p == 0.0) return 0.0;
            if (p == 1.0) return 1.0;
            return pow(2.0, -10.0 * p) * sin((p * 10.0 - 0.75) * c4) + 1.0;
        }
        case EASE_IN_OUT_ELASTIC: {
            double c5 = (2.0 * EASE_PI) / 4.5;
            if (p == 0.0) return 0.0;
            if (p == 1.0) return 1.0;
            if (p < 0.5) return -(pow(2.0, 20.0 * p - 10.0) * sin((20.0 * p - 11.125) * c5)) / 2.0;
            return (pow(2.0, -20.0 * p + 10.0) * sin((20.0 * p - 11.125) * c5)) / 2.0 + 1.0;
        }
        case EASE_IN_BOUNCE:
            return 1.0 - out_bounce(1.0 - p);
        case EASE_OUT_BOUNCE:
            return out_bounce(p);
        case EASE_IN_OUT_BOUNCE:
            if (p < 0.5) return (1.0 - out_bounce(1.0 - 2.0 * p)) / 2.0;
            return (1.0 + out_bounce(2.0 * p - 1.0)) / 2.0;
        case EASE_CUBIC_BEZIER:
            return bezier_easing(e->x1, e->y1, e->x2, e->y2, p);
    }
    return p;
}

void easing_tracker_init(EasingTracker *t, Easing easing_function, int64_t total_steps, bool clamp) {
    memset(t, 0, sizeof(*t));
    t->easing_function = easing_function;
    t->total_steps = total_steps;
    t->clamp = clamp;
}

double easing_tracker_step(EasingTracker *t) {
    if (t->current_step < t->total_steps) {
        t->current_step += 1;
        t->progress_ratio = (double)t->current_step / (double)t->total_steps;
        t->eased_value = easing_ease(&t->easing_function, t->progress_ratio);
        if (t->clamp) {
            if (t->eased_value > 1.0) t->eased_value = 1.0;
            if (t->eased_value < 0.0) t->eased_value = 0.0;
        }
        t->step_delta = t->eased_value - t->last_eased_value;
        t->last_eased_value = t->eased_value;
    }
    return t->eased_value;
}

void easing_tracker_reset(EasingTracker *t) {
    t->current_step = 0;
    t->progress_ratio = 0.0;
    t->step_delta = 0.0;
    t->eased_value = 0.0;
    t->last_eased_value = 0.0;
}

bool easing_tracker_is_complete(const EasingTracker *t) {
    return t->current_step >= t->total_steps;
}
