// SequenceEaser over character groups, ported from the reference
// utils/easing.py SequenceEaser. The sequence is owned by the caller.
#ifndef GLYPHFX_SEQUENCE_EASER_H
#define GLYPHFX_SEQUENCE_EASER_H

#include <stdbool.h>
#include <stddef.h>

#include "engine/terminal.h"
#include "utils/easing.h"

typedef struct {
    CharIdBucket *sequence;
    size_t len;
    EasingTracker tracker;
} SequenceEaser;

typedef struct {
    CharIdBucket *added;
    size_t added_len;
    CharIdBucket *removed;
    size_t removed_len;
} SequenceStep;

void sequence_easer_init(SequenceEaser *e, CharIdBucket *sequence, size_t len, Easing easing, int64_t total_steps);
SequenceStep sequence_easer_step(SequenceEaser *e);
bool sequence_easer_is_complete(const SequenceEaser *e);
void sequence_easer_reset(SequenceEaser *e);

#endif
