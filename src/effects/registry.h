// Static effect registry (name -> option table + constructor), replacing
// upstream's dynamic discovery. Each effect file defines a strong
// <name>_entry; the registry declares them weak so a partial build links and
// can be exercised effect by effect.
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
    void (*free_config)(void *cfg);
    Effect *(*make)(const void *cfg);
} EffectEntry;

#if defined(__GNUC__) || defined(__clang__)
#define GLYPHFX_WEAK __attribute__((weak))
#else
#define GLYPHFX_WEAK
#endif

#define GLYPHFX_EFFECT_NAMES(X)      \
    X(beams)                         \
    X(binarypath)                    \
    X(blackhole)                     \
    X(bouncyballs)                   \
    X(bubbles)                       \
    X(burn)                          \
    X(colorshift)                    \
    X(crumble)                       \
    X(decrypt)                       \
    X(errorcorrect)                  \
    X(expand)                        \
    X(fireworks)                     \
    X(highlight)                     \
    X(laseretch)                     \
    X(matrix)                        \
    X(middleout)                     \
    X(orbittingvolley)               \
    X(overflow)                      \
    X(pour)                          \
    X(print)                         \
    X(rain)                          \
    X(randomsequence)                \
    X(rings)                         \
    X(scattered)                     \
    X(slice)                         \
    X(slide)                         \
    X(smoke)                         \
    X(spotlights)                    \
    X(spray)                         \
    X(swarm)                         \
    X(sweep)                         \
    X(synthgrid)                     \
    X(thunderstorm)                  \
    X(unstable)                      \
    X(vhstape)                       \
    X(waves)                         \
    X(wipe)

#define GLYPHFX_DECLARE_ENTRY(name) extern const EffectEntry name##_entry GLYPHFX_WEAK;
GLYPHFX_EFFECT_NAMES(GLYPHFX_DECLARE_ENTRY)

const EffectEntry *effect_find(const char *name);
const EffectEntry *effect_entry_at(size_t index);
size_t effect_entry_count(void);

#endif
