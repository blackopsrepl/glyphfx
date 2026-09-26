#include "effects/registry.h"

#include <string.h>

#include "effects/random_sequence.h"

static const EffectEntry EFFECTS[] = {
    {
        "randomsequence",
        randomsequence_specs,
        0,  // n_specs filled below via randomsequence_specs_len
        sizeof(RandomSequenceConfig),
        randomsequence_config_defaults,
        randomsequence_free_config,
        randomsequence_make,
    },
};

#define EFFECT_COUNT (sizeof(EFFECTS) / sizeof(EFFECTS[0]))

static size_t entry_n_specs(size_t index) {
    switch (index) {
        case 0:
            return randomsequence_specs_len;
        default:
            return 0;
    }
}

size_t effect_entry_count(void) {
    return EFFECT_COUNT;
}

const EffectEntry *effect_entry_at(size_t index) {
    if (index >= EFFECT_COUNT) {
        return NULL;
    }
    static EffectEntry resolved;
    resolved = EFFECTS[index];
    resolved.n_specs = entry_n_specs(index);
    return &resolved;
}

const EffectEntry *effect_find(const char *name) {
    for (size_t i = 0; i < EFFECT_COUNT; i++) {
        if (strcmp(EFFECTS[i].name, name) == 0) {
            return effect_entry_at(i);
        }
    }
    return NULL;
}
