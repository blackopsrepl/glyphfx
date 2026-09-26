#include "effects/registry.h"

#include <string.h>

static const EffectEntry *const EFFECTS[] = {
#define GLYPHFX_ENTRY_PTR(name) &name##_entry,
    GLYPHFX_EFFECT_NAMES(GLYPHFX_ENTRY_PTR)
#undef GLYPHFX_ENTRY_PTR
};

#define EFFECT_COUNT (sizeof(EFFECTS) / sizeof(EFFECTS[0]))

size_t effect_entry_count(void) {
    return EFFECT_COUNT;
}

const EffectEntry *effect_entry_at(size_t index) {
    if (index >= EFFECT_COUNT) {
        return NULL;
    }
    const EffectEntry *entry = EFFECTS[index];
    if (entry == NULL || entry->name == NULL) {
        return NULL;
    }
    return entry;
}

const EffectEntry *effect_find(const char *name) {
    for (size_t i = 0; i < EFFECT_COUNT; i++) {
        const EffectEntry *entry = EFFECTS[i];
        if (entry != NULL && entry->name != NULL && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}
