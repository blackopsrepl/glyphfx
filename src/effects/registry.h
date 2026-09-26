// Static effect registry (name -> option table + constructor). Replaces
// upstream's dynamic effect discovery.
#ifndef GLYPHFX_REGISTRY_H
#define GLYPHFX_REGISTRY_H

#include <stddef.h>

#include "cli.h"
#include "engine/effect.h"

typedef struct {
    const char *name;
    const EffOptSpec *specs;
    size_t n_specs;
    size_t config_size;
    void (*defaults)(void *cfg);
    // Frees any heap owned by the config (e.g. option lists); the config
    // struct itself is freed by the caller.
    void (*free_config)(void *cfg);
    Effect *(*make)(const void *cfg);
} EffectEntry;

const EffectEntry *effect_find(const char *name);
const EffectEntry *effect_entry_at(size_t index);
size_t effect_entry_count(void);

#endif
