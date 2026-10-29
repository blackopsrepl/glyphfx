#include "engine/doze.h"

#include <stdlib.h>
#include <string.h>

#include "engine/animation.h"
#include "engine/ctx.h"

enum { DOZE_PLAIN = 0, DOZE_EASED = 1 };

// The longest doze computed at once. A doze re-arms on the tick that ends it,
// so the cap only bounds the look-ahead walk, not the skipped work. Builds may
// lower it (-DDOZE_MAX_TICKS=1) to exercise many more doze boundaries.
#ifndef DOZE_MAX_TICKS
#define DOZE_MAX_TICKS 4096
#endif
#ifndef DOZE_MIN_TICKS
#define DOZE_MIN_TICKS 1
#endif

Doze *g_doze;

void doze_init(Doze *d, struct EngineCtx *ctx) {
    memset(d, 0, sizeof(*d));
    d->cursor = CHAR_ID_NONE;
    d->ctx = ctx;
}

void doze_free(Doze *d) {
    if (g_doze == d) {
        g_doze = NULL;
    }
    free(d->bits);
    free(d->snapshot);
    free(d->from);
    free(d->until);
    free(d->kind);
    memset(d, 0, sizeof(*d));
    d->cursor = CHAR_ID_NONE;
}

static bool grow_array(void **p, size_t elem, size_t old_n, size_t new_n) {
    void *grown = realloc(*p, new_n * elem);
    if (!grown) {
        return false;
    }
    memset((char *)grown + old_n * elem, 0, (new_n - old_n) * elem);
    *p = grown;
    return true;
}

bool doze_grow(Doze *d, size_t slots) {
    if (slots <= d->nslots) {
        return true;
    }
    size_t cap = d->nslots ? d->nslots : 1024;
    while (cap < slots) {
        cap *= 2;
    }
    size_t words = (cap + 63) / 64;
    if (!grow_array((void **)&d->from, sizeof(uint32_t), d->nslots, cap) ||
        !grow_array((void **)&d->until, sizeof(uint32_t), d->nslots, cap) ||
        !grow_array((void **)&d->kind, sizeof(uint8_t), d->nslots, cap) ||
        !grow_array((void **)&d->bits, sizeof(uint64_t), d->nwords, words) ||
        !grow_array((void **)&d->snapshot, sizeof(uint64_t), d->nwords, words)) {
        return false;
    }
    d->nwords = words;
    d->nslots = cap;
    return true;
}

static Scene *dozing_scene(EngineCtx *ctx, CharId id) {
    Animation *anim = &ctx->terminal.arena.items[id].animation;
    if (!anim->active_scene) {
        return NULL;
    }
    return animation_active_scene(anim);
}

// Pure ticks ahead of a plain scene: the head frame's remaining ticks while
// its visual is the one shown, across frame retirements, stopping before the
// last frame's retiring tick (which completes the scene).
static uint32_t plain_pure_ticks(const Scene *scene, VisualHandle shown) {
    uint64_t k = 0;
    size_t j = scene->head;
    int64_t elapsed = scene->all_frames[j].ticks_elapsed;
    while (k < DOZE_MAX_TICKS) {
        const Frame *f = &scene->all_frames[j];
        if (f->visual != shown) {
            break;
        }
        int64_t rem = f->duration - elapsed;  // >= 1: durations are >= 1
        if (rem < 1) {
            break;  // never reached: ticks_elapsed resets when it hits duration
        }
        if (j + 1 == scene->all_frames_len) {
            k += (uint64_t)(rem - 1);
            break;
        }
        k += (uint64_t)rem;
        j++;
        elapsed = 0;
    }
    return (uint32_t)(k < DOZE_MAX_TICKS ? k : DOZE_MAX_TICKS);
}

// Pure ticks ahead of an eased scene: steps whose frame shows the visual
// already shown (or no frame at all), stopping before the step that reaches
// easing_total_steps (which completes a non-looping scene).
static uint32_t eased_pure_ticks(const Scene *scene, VisualHandle shown) {
    int64_t total = scene->easing_total_steps;
    int64_t step = scene->easing_current_step;
    if (total <= 0 || step < 0 || step + 1 >= total) {
        return 0;
    }
    const int32_t *seq = eased_frame_seq(&scene->ease, total);
    if (!seq) {
        return 0;
    }
    uint32_t k = 0;
    while (k < DOZE_MAX_TICKS && step + 1 < total) {
        int64_t idx = seq[step];
        if (idx >= 0 && (size_t)idx < scene->frame_index_map_len &&
            scene->all_frames[scene->frame_index_map[idx]].visual != shown) {
            break;
        }
        k++;
        step++;
    }
    return k;
}

void doze_try(Doze *d, CharId id) {
    EngineCtx *ctx = d->ctx;
    if ((size_t)id >= d->nslots || !ac_contains(&ctx->active_characters, id)) {
        return;
    }
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    if (ch->motion.active_path) {
        return;
    }
    Scene *scene = dozing_scene(ctx, id);
    if (!scene || scene->is_looping || scene->has_sync || scene->head >= scene->all_frames_len) {
        return;
    }
    VisualHandle shown = ch->animation.current_visual;
    uint32_t k;
    uint8_t kind;
    if (scene->has_ease) {
        k = eased_pure_ticks(scene, shown);
        kind = DOZE_EASED;
    } else {
        k = plain_pure_ticks(scene, shown);
        kind = DOZE_PLAIN;
    }
    if (k < DOZE_MIN_TICKS) {
        return;
    }
    d->bits[(size_t)id >> 6] |= 1ULL << (id & 63);
    d->from[id] = d->count;
    d->until[id] = d->count + k;
    d->kind[id] = kind;
    d->live++;
}

static void apply_pure_ticks(Scene *scene, uint8_t kind, uint32_t n) {
    if (kind == DOZE_EASED) {
        scene->easing_current_step += n;
        return;
    }
    while (n) {
        Frame *f = &scene->all_frames[scene->head];
        uint64_t rem = (uint64_t)(f->duration - f->ticks_elapsed);
        if (n < rem) {
            f->ticks_elapsed += n;
            return;
        }
        n -= (uint32_t)rem;
        f->ticks_elapsed = 0;
        scene->head++;
    }
}

// Apply the pure ticks the old engine would have run through update `last`.
static void settle_through(Doze *d, CharId id, uint32_t last) {
    d->bits[(size_t)id >> 6] &= ~(1ULL << (id & 63));
    d->live--;
    uint32_t end = last < d->until[id] ? last : d->until[id];
    uint32_t owed = end - d->from[id];
    if (owed == 0) {
        return;
    }
    Scene *scene = dozing_scene(d->ctx, id);
    if (scene) {
        apply_pure_ticks(scene, d->kind[id], owed);
    }
}

void doze_settle(Doze *d, CharId id) {
    settle_through(d, id, d->until[id]);
}

void doze_wake_all(void) {
    Doze *d = g_doze;
    if (!d || !d->live) {
        return;
    }
    for (size_t w = 0; w < d->nwords && d->live; w++) {
        uint64_t m = d->bits[w];
        while (m) {
            CharId id = (CharId)((w << 6) | (unsigned)__builtin_ctzll(m));
            m &= m - 1;
            doze_wake_slow(id);
        }
    }
}

void doze_wake_slow(CharId id) {
    Doze *d = g_doze;
    // A character after the cursor is still due its tick in this update (it
    // was active when the update began): it rejoins the snapshot and owes the
    // ticks before this update. Any other owes this update's tick too.
    bool rejoin = d->in_update && id > d->cursor;
    uint32_t last = rejoin ? d->count - 1 : d->count;
    settle_through(d, id, last);
    if (rejoin && (size_t)id >> 6 < d->snapshot_words) {
        d->snapshot[(size_t)id >> 6] |= 1ULL << (id & 63);
    }
}
