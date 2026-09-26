#include "engine/particles.h"

#include <stdlib.h>
#include <string.h>

ParticleReset particle_reset_default(void) {
    ParticleReset r;
    r.clear_paths = true;
    r.clear_scenes = false;
    r.clear_events = false;
    r.deactivate_path = true;
    r.deactivate_scene = true;
    r.reset_appearance = false;
    return r;
}

int particle_pool_init(ParticlePool *pool, const char *const *symbols, size_t n_symbols, bool has_max_size,
                       size_t max_size, Coord coord) {
    memset(pool, 0, sizeof(*pool));
    if (n_symbols == 0) {
        return -1;
    }
    pool->symbols = malloc(n_symbols * sizeof(char *));
    for (size_t i = 0; i < n_symbols; i++) {
        pool->symbols[i] = malloc(strlen(symbols[i]) + 1);
        strcpy(pool->symbols[i], symbols[i]);
    }
    pool->symbols_len = n_symbols;
    pool->has_max_size = has_max_size;
    pool->max_size = max_size;
    pool->coord = coord;
    return 0;
}

void particle_pool_free(ParticlePool *pool) {
    for (size_t i = 0; i < pool->symbols_len; i++) {
        free(pool->symbols[i]);
    }
    free(pool->symbols);
    free(pool->available);
    free(pool->particles);
    memset(pool, 0, sizeof(*pool));
}

static void available_push(ParticlePool *pool, CharId id) {
    if (pool->available_len == pool->available_cap) {
        size_t cap = pool->available_cap ? pool->available_cap * 2 : 16;
        pool->available = realloc(pool->available, cap * sizeof(CharId));
        pool->available_cap = cap;
    }
    pool->available[pool->available_len++] = id;
}

static CharId available_pop(ParticlePool *pool) {
    if (pool->available_len == 0) {
        return CHAR_ID_NONE;
    }
    return pool->available[--pool->available_len];
}

static bool available_contains(const ParticlePool *pool, CharId id) {
    for (size_t i = 0; i < pool->available_len; i++) {
        if (pool->available[i] == id) {
            return true;
        }
    }
    return false;
}

static void particles_push(ParticlePool *pool, CharId id) {
    if (pool->particles_len == pool->particles_cap) {
        size_t cap = pool->particles_cap ? pool->particles_cap * 2 : 16;
        pool->particles = realloc(pool->particles, cap * sizeof(CharId));
        pool->particles_cap = cap;
    }
    pool->particles[pool->particles_len++] = id;
}

size_t particle_pool_len(const ParticlePool *pool) {
    return pool->particles_len;
}

bool particle_pool_is_empty(const ParticlePool *pool) {
    return pool->particles_len == 0;
}

static void reset_particle(EngineCtx *ctx, CharId id, ParticleReset reset) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    if (reset.deactivate_path) {
        motion_deactivate_path(&ch->motion, NULL);
    }
    if (reset.deactivate_scene) {
        free(ch->animation.active_scene);
        ch->animation.active_scene = NULL;
    }
    if (reset.clear_paths) {
        motion_clear_paths(&ch->motion);
    }
    if (reset.clear_scenes) {
        animation_clear_scenes(&ch->animation);
    }
    if (reset.clear_events) {
        event_handler_clear(&ch->event_handler);
    }
    if (reset.reset_appearance) {
        animation_set_appearance(&ch->animation, ch->uses_input_preexisting_colors, ch->input_symbol, NULL);
    }
}

static CharId create_particle(ParticlePool *pool, EngineCtx *ctx, const char *symbol, ParticleFn initializer,
                              void *user) {
    const char *use_symbol;
    if (symbol) {
        use_symbol = symbol;
    } else {
        use_symbol = pool->symbols[rng_choice_index(&ctx->rng, pool->symbols_len)];
    }
    CharId particle = terminal_add_character(&ctx->terminal, use_symbol, pool->coord);
    if (initializer) {
        initializer(user, ctx, particle);
    }
    particles_push(pool, particle);
    return particle;
}

int particle_pool_preallocate(ParticlePool *pool, EngineCtx *ctx, size_t initial_count, ParticleFn initializer,
                              void *user) {
    if (pool->has_max_size && pool->max_size < initial_count) {
        return -1;
    }
    for (size_t i = 0; i < initial_count; i++) {
        CharId p = create_particle(pool, ctx, NULL, initializer, user);
        available_push(pool, p);
    }
    return 0;
}

CharId particle_pool_acquire(ParticlePool *pool, EngineCtx *ctx, const char *symbol, ParticleReset reset,
                             ParticleFn initializer, void *init_user) {
    CharId particle = available_pop(pool);
    if (particle != CHAR_ID_NONE) {
        reset_particle(ctx, particle, reset);
        if (symbol) {
            EffectCharacter *ch = &ctx->terminal.arena.items[particle];
            free(ch->input_symbol);
            ch->input_symbol = malloc(strlen(symbol) + 1);
            strcpy(ch->input_symbol, symbol);
            animation_set_appearance(&ch->animation, ch->uses_input_preexisting_colors, symbol, NULL);
        }
        return particle;
    }
    if (pool->has_max_size && pool->particles_len >= pool->max_size) {
        return CHAR_ID_NONE;
    }
    particle = create_particle(pool, ctx, symbol, initializer, init_user);
    reset_particle(ctx, particle, reset);
    return particle;
}

CharId particle_pool_emit(ParticlePool *pool, EngineCtx *ctx, Coord origin, const char *symbol, bool visible,
                          ParticleReset reset, ParticleFn initializer, void *init_user, ParticleFn on_emit,
                          void *on_emit_user) {
    CharId particle = particle_pool_acquire(pool, ctx, symbol, reset, initializer, init_user);
    if (particle == CHAR_ID_NONE) {
        return CHAR_ID_NONE;
    }
    motion_set_coordinate(&ctx->terminal.arena.items[particle].motion, origin);
    if (on_emit) {
        on_emit(on_emit_user, ctx, particle);
    }
    terminal_set_character_visibility(&ctx->terminal, particle, visible);
    ac_insert(&ctx->active_characters, particle);
    return particle;
}

void particle_pool_reclaim(ParticlePool *pool, EngineCtx *ctx, CharId id, bool hide, bool deactivate) {
    if (hide) {
        terminal_set_character_visibility(&ctx->terminal, id, false);
    }
    if (deactivate) {
        EffectCharacter *ch = &ctx->terminal.arena.items[id];
        motion_deactivate_path(&ch->motion, NULL);
        free(ch->animation.active_scene);
        ch->animation.active_scene = NULL;
    }
    ac_remove(&ctx->active_characters, id);
    if (!available_contains(pool, id)) {
        available_push(pool, id);
    }
}

void particle_pool_extend(ParticlePool *pool, const CharId *ids, size_t n) {
    for (size_t i = 0; i < n; i++) {
        particles_push(pool, ids[i]);
        available_push(pool, ids[i]);
    }
}
