// Injected clock. Production reads real time; the parity path advances a fixed
// 1/frame_rate per emitted frame, so clock-dependent effects are reproducible.
#ifndef GLYPHFX_CLOCK_H
#define GLYPHFX_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CLOCK_REAL,
    CLOCK_VIRTUAL,
} ClockKind;

typedef struct {
    ClockKind kind;
    double start_mono;  // real: monotonic seconds at construction
    double wall_start;  // real: wall seconds at construction
    double now;         // virtual: current virtual seconds
    double dt;          // virtual: advance per frame
} Clock;

Clock clock_real(void);
Clock clock_virtual_with_frame_rate(int64_t frame_rate);
double clock_now_wall(const Clock *c);
double clock_now_monotonic(const Clock *c);
void clock_advance_frame(Clock *c);
static inline bool clock_is_real(const Clock *c) {
    return c->kind == CLOCK_REAL;
}

#endif
