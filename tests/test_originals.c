// glyphfx originals: registry placement and the shared colour helpers.
#include <string.h>

#include "effects/bannerwave.h"
#include "effects/burnout.h"
#include "effects/comet.h"
#include "effects/copper.h"
#include "effects/originals.h"
#include "effects/registry.h"
#include "testutil.h"

// The registry declares entries weak, and a weak reference does not pull an
// object out of libglyphfx.a; these strong references link the four effects
// (volatile, so the optimiser cannot fold the references away).
static Effect *(*volatile LINKED[])(const void *) = {bannerwave_make, burnout_make, comet_make, copper_make};

static const char *const ORIGINALS[] = {"bannerwave", "burnout", "comet", "copper"};

static Color hex(const char *h) {
    Color c;
    color_from_hex(h, &c);
    return c;
}

static int rgb_eq(const Color *c, int r, int g, int b) {
    uint8_t v[3];
    color_rgb_ints(c, v);
    return v[0] == r && v[1] == g && v[2] == b;
}

int main(void) {
    CHECK(LINKED[0] && LINKED[1] && LINKED[2] && LINKED[3]);
    // The ttfx set keeps its 37 slots ahead of the originals. Only the
    // originals are linked into this binary, so the ttfx slots read as NULL.
    CHECK_EQ_INT(effect_parity_count(), 37);
    CHECK_EQ_INT(effect_entry_count(), 37 + 4);
    for (size_t i = 0; i < effect_parity_count(); i++) {
        CHECK(!effect_is_original(effect_entry_at(i)));
    }
    for (size_t i = 0; i < 4; i++) {
        const EffectEntry *e = effect_find(ORIGINALS[i]);
        CHECK(e != NULL);
        CHECK(effect_is_original(e));
        CHECK(e == effect_entry_at(effect_parity_count() + i));
        // Every option table entry is reachable and named.
        for (size_t k = 0; e && k < e->n_specs; k++) {
            CHECK(e->specs[k].name != NULL && e->specs[k].name[0] != '\0');
        }
    }
    CHECK(!effect_is_original(NULL));

    // Mixes snap to ORIG_MIX_STEPS and hit both endpoints exactly.
    Color black = hex("000000"), white = hex("ffffff");
    Color m0 = orig_mix(&black, &white, 0.0);
    Color m1 = orig_mix(&black, &white, 1.0);
    Color mh = orig_mix(&black, &white, 0.5);
    Color mq = orig_mix(&black, &white, 0.51);
    CHECK(rgb_eq(&m0, 0, 0, 0));
    CHECK(rgb_eq(&m1, 255, 255, 255));
    CHECK(rgb_eq(&mh, 128, 128, 128));
    CHECK(rgb_eq(&mq, 128, 128, 128));
    Color under = orig_mix(&black, &white, -3.0), over = orig_mix(&black, &white, 7.0);
    CHECK(rgb_eq(&under, 0, 0, 0));
    CHECK(rgb_eq(&over, 255, 255, 255));

    // A three-stop ramp passes through its middle stop.
    Color stops[3] = {hex("ff0000"), hex("00ff00"), hex("0000ff")};
    Color r0 = orig_ramp(stops, 3, 0.0), r5 = orig_ramp(stops, 3, 0.5), r1 = orig_ramp(stops, 3, 1.0);
    CHECK(rgb_eq(&r0, 255, 0, 0));
    CHECK(rgb_eq(&r5, 0, 255, 0));
    CHECK(rgb_eq(&r1, 0, 0, 255));
    Color single = orig_ramp(stops, 1, 0.7);
    CHECK(rgb_eq(&single, 255, 0, 0));

    CHECK(orig_smooth(-1.0) == 0.0);
    CHECK(orig_smooth(2.0) == 1.0);
    CHECK(orig_smooth(0.5) == 0.5);
    return test_summary("test_originals");
}
