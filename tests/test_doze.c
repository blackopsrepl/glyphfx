#include "engine/doze.h"
#include "utils/rng.h"
#include "testutil.h"

static void check_mask(const uint32_t *until, uint32_t now, uint64_t dozing) {
    uint64_t expected = 0;
    for (unsigned lane = 0; lane < 64; lane++) {
        if (until[lane] >= now) expected |= UINT64_C(1) << lane;
    }
    CHECK(doze_pending_word(until, now, dozing) == (expected & dozing));
}

int main(void) {
    uint32_t until[64];
    const uint32_t edges[] = {0, 1, INT32_MAX, UINT32_C(0x80000000), UINT32_MAX - 1, UINT32_MAX};
    for (size_t end = 0; end < sizeof(edges) / sizeof(edges[0]); end++) {
        for (unsigned lane = 0; lane < 64; lane++) until[lane] = edges[end];
        for (size_t now = 0; now < sizeof(edges) / sizeof(edges[0]); now++) {
            check_mask(until, edges[now], UINT64_MAX);
            check_mask(until, edges[now], 0);
            for (unsigned lane = 0; lane < 64; lane++) {
                check_mask(until, edges[now], UINT64_C(1) << lane);
            }
        }
    }
    Rng rng = rng_seeded(1);
    for (unsigned rep = 0; rep < 10000; rep++) {
        uint32_t now = (uint32_t)rng_next_u64(&rng);
        for (unsigned lane = 0; lane < 64; lane++) {
            until[lane] = lane % 3 == 0 ? now : (uint32_t)rng_next_u64(&rng);
        }
        check_mask(until, now, rng_next_u64(&rng));
        check_mask(until, now, UINT64_MAX);
        check_mask(until, now, UINT64_C(0x8000000000000001));
        check_mask(until, now, UINT64_C(0x8000000000000003));
    }
    return test_summary("test_doze");
}
