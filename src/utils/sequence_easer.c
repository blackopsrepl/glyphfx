#include "utils/sequence_easer.h"

void sequence_easer_init(SequenceEaser *e, CharIdBucket *sequence, size_t len, Easing easing, int64_t total_steps) {
    e->sequence = sequence;
    e->len = len;
    easing_tracker_init(&e->tracker, easing, total_steps, true);
}

SequenceStep sequence_easer_step(SequenceEaser *e) {
    SequenceStep step;
    step.added = NULL;
    step.added_len = 0;
    step.removed = NULL;
    step.removed_len = 0;
    double previous_eased = e->tracker.eased_value;
    double eased_value = easing_tracker_step(&e->tracker);
    if (e->len == 0) {
        return step;
    }
    size_t length = (size_t)(int64_t)(eased_value * (double)e->len);
    size_t previous_length = (size_t)(int64_t)(previous_eased * (double)e->len);
    if (length > previous_length) {
        step.added = &e->sequence[previous_length];
        step.added_len = length - previous_length;
    } else if (length < previous_length) {
        step.removed = &e->sequence[length];
        step.removed_len = previous_length - length;
    }
    return step;
}

bool sequence_easer_is_complete(const SequenceEaser *e) {
    return easing_tracker_is_complete(&e->tracker);
}

void sequence_easer_reset(SequenceEaser *e) {
    easing_tracker_reset(&e->tracker);
}
