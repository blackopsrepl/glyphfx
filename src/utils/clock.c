#include "utils/clock.h"

#include <time.h>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

static double monotonic_seconds(void) {
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#elif defined(__APPLE__)
    return (double)mach_absolute_time() / 1e9;
#else
    return (double)clock() / (double)CLOCKS_PER_SEC;
#endif
}

static double wall_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

Clock clock_real(void) {
    Clock c;
    c.kind = CLOCK_REAL;
    c.start_mono = monotonic_seconds();
    c.wall_start = wall_seconds();
    c.now = 0.0;
    c.dt = 0.0;
    return c;
}

Clock clock_virtual_with_frame_rate(int64_t frame_rate) {
    Clock c;
    c.kind = CLOCK_VIRTUAL;
    c.start_mono = 0.0;
    c.wall_start = 0.0;
    c.now = 0.0;
    c.dt = frame_rate > 0 ? 1.0 / (double)frame_rate : 1.0 / 60.0;
    return c;
}

double clock_now_wall(const Clock *c) {
    if (c->kind == CLOCK_VIRTUAL) {
        return c->now;
    }
    return c->wall_start + (monotonic_seconds() - c->start_mono);
}

double clock_now_monotonic(const Clock *c) {
    if (c->kind == CLOCK_VIRTUAL) {
        return c->now;
    }
    return monotonic_seconds() - c->start_mono;
}

void clock_advance_frame(Clock *c) {
    if (c->kind == CLOCK_VIRTUAL) {
        c->now += c->dt;
    }
}
