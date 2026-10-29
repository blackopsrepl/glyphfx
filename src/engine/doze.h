// Dozing: skip the ticks that cannot be observed.
//
// Most ticks of most characters only count down. A character with no active
// path whose active scene is a plain or eased (not synced, not looping) scene
// usually spends its tick advancing a frame counter while it keeps showing the
// visual it already shows: no movement, no new visual, no event. Such a tick
// is pure, and so is a run of them, up to (not including) the tick that would
// change the visual or complete the scene.
//
// After engine_update ticks a character, doze_try counts the pure ticks ahead
// (k) and parks the character for the next k updates: it stays in the active
// set (it is active, and nothing about that can change while it dozes) but out
// of the update snapshots. Its scene counters are left as they are and settled
// lazily: on the update that takes it back, or as soon as anything could
// observe or change its playback state (doze_wake), the owed pure ticks are
// applied at once. Applying them is exact because a pure tick only advances
// ticks_elapsed/head (plain) or the eased step, never past the scene's end.
//
// The rule that keeps this byte-exact: every path that reads or rewrites a
// character's playback state, gives it a path, or ticks it outside
// engine_update calls doze_wake first. Inside the engine those are scene
// activation and deactivation, path activation, set_appearance, scene_reset,
// adding frames, clearing scenes, particle reset/reclaim, and removing from or
// clearing the active set. An effect that writes Scene fields directly must
// call scene_wake on that scene first.
//
// A character woken during engine_update ahead of the character being ticked
// rejoins this update's snapshot, so it ticks exactly where an engine without
// dozing would tick it. Arena slot == character_id (both allocated in
// lockstep), so Animation.render_id names the slot.
//
// The hooks without a context reach the dozes of the bound context (g_doze,
// see engine_bind). An effect's next_frame changes scenes before its
// engine_update binds, so a host interleaving contexts must bind first: a hook
// that reached another context would miss a wake here.
#ifndef GLYPHFX_DOZE_H
#define GLYPHFX_DOZE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine/charid.h"

struct EngineCtx;

typedef struct {
    uint64_t *bits;      // dozing characters
    uint64_t *snapshot;  // the running update's snapshot of the active set
    size_t nwords;       // words in bits and snapshot
    size_t snapshot_words;  // words of the running update's snapshot
    uint32_t *from;      // the update whose tick started the doze
    uint32_t *until;     // the last update the doze covers
    uint8_t *kind;       // DOZE_PLAIN or DOZE_EASED
    size_t nslots;
    size_t live;         // dozing characters, so the wake check is one load when none
    uint32_t count;      // updates started
    bool in_update;
    CharId cursor;       // the character being ticked by engine_update
    struct EngineCtx *ctx;
} Doze;

extern Doze *g_doze;

void doze_init(Doze *d, struct EngineCtx *ctx);
void doze_free(Doze *d);
// Room for `slots` characters; false on allocation failure.
bool doze_grow(Doze *d, size_t slots);
// After engine_update ticked `id`: park it through its pure ticks, if any.
void doze_try(Doze *d, CharId id);
// Settle a doze: apply the owed pure ticks. engine_update calls this for a
// character whose doze ran out; doze_wake for every other end.
void doze_settle(Doze *d, CharId id);

void doze_wake_slow(CharId id);
// End every doze (the active set is being cleared).
void doze_wake_all(void);

static inline bool doze_is_dozing(const Doze *d, CharId id) {
    return (size_t)id < d->nslots && ((d->bits[(size_t)id >> 6] >> (id & 63)) & 1u);
}

// End `id`'s doze, if any, before its playback state is read or changed.
static inline void doze_wake(CharId id) {
    if (g_doze && g_doze->live && id >= 0 && doze_is_dozing(g_doze, id)) {
        doze_wake_slow(id);
    }
}

#endif
