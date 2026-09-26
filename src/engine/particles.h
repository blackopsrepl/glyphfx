// ParticlePool / ParticleReset, ported from engine/effect_support/particles.py.
// The pool lives in effect state and operates through EngineCtx; the upstream
// initializer/on_emit closures become function pointers with a user pointer.
#ifndef GLYPHFX_PARTICLES_H
#define GLYPHFX_PARTICLES_H

#include <stdbool.h>
#include <stddef.h>

#include "engine/ctx.h"

typedef struct {
    bool clear_paths;
    bool clear_scenes;
    bool clear_events;
    bool deactivate_path;
    bool deactivate_scene;
    bool reset_appearance;
} ParticleReset;

ParticleReset particle_reset_default(void);

typedef void (*ParticleFn)(void *user, EngineCtx *ctx, CharId id);

typedef struct {
    char **symbols;
    size_t symbols_len;
    bool has_max_size;
    size_t max_size;
    Coord coord;
    CharId *available;  // stack: pop/push at the back
    size_t available_len;
    size_t available_cap;
    CharId *particles;
    size_t particles_len;
    size_t particles_cap;
} ParticlePool;

int particle_pool_init(ParticlePool *pool, const char *const *symbols, size_t n_symbols, bool has_max_size,
                       size_t max_size, Coord coord);
void particle_pool_free(ParticlePool *pool);
int particle_pool_preallocate(ParticlePool *pool, EngineCtx *ctx, size_t initial_count, ParticleFn initializer,
                              void *user);
size_t particle_pool_len(const ParticlePool *pool);
bool particle_pool_is_empty(const ParticlePool *pool);
// Returns CHAR_ID_NONE when the pool is exhausted.
CharId particle_pool_acquire(ParticlePool *pool, EngineCtx *ctx, const char *symbol, ParticleReset reset,
                             ParticleFn initializer, void *init_user);
CharId particle_pool_emit(ParticlePool *pool, EngineCtx *ctx, Coord origin, const char *symbol, bool visible,
                          ParticleReset reset, ParticleFn initializer, void *init_user, ParticleFn on_emit,
                          void *on_emit_user);
void particle_pool_reclaim(ParticlePool *pool, EngineCtx *ctx, CharId id, bool hide, bool deactivate);
void particle_pool_extend(ParticlePool *pool, const CharId *ids, size_t n);

#endif
