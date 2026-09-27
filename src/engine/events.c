#include "engine/events.h"

#include <stdlib.h>
#include <string.h>

#include "utils/strhash.h"
#include "utils/strtab.h"

static char *dup_cstr(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, len + 1);
    return out;
}

void caller_key_free(CallerKey *key) {
    free(key->waypoint.bezier);
    memset(key, 0, sizeof(*key));
}

void caller_key_copy(CallerKey *dst, const CallerKey *src) {
    memset(dst, 0, sizeof(*dst));
    dst->kind = src->kind;
    // The stored key needs a stable name: effects build caller keys from stack
    // buffers, so the copy interns the name (which is process-lifetime) and
    // keys on its handle. Equal handles mean equal names.
    dst->id = (char *)strtab_intern(src->id);
    dst->id_handle = src->id_handle ? src->id_handle : strtab_handle(src->id);
    if (src->kind == CALLER_WAYPOINT) {
        dst->waypoint.coord = src->waypoint.coord;
        dst->waypoint.waypoint_id = (char *)strtab_intern(src->waypoint.waypoint_id);
        dst->waypoint.waypoint_handle = src->waypoint.waypoint_handle
                                            ? src->waypoint.waypoint_handle
                                            : strtab_handle(src->waypoint.waypoint_id);
        dst->waypoint.bezier_len = src->waypoint.bezier_len;
        if (src->waypoint.bezier_len) {
            dst->waypoint.bezier = malloc(src->waypoint.bezier_len * sizeof(Coord));
            memcpy(dst->waypoint.bezier, src->waypoint.bezier, src->waypoint.bezier_len * sizeof(Coord));
        }
    }
}

bool caller_key_matches(const CallerKey *a, const CallerKey *b) {
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
        case CALLER_SCENE:
        case CALLER_PATH:
            if (!a->id || !b->id) {
                return false;
            }
            // Both handles are set once a key has passed through the engine,
            // so a scan compares integers; the string compare is the fallback
            // for a raw effect-built key.
            if (a->id_handle && b->id_handle) {
                return a->id_handle == b->id_handle;
            }
            return strcmp(a->id, b->id) == 0;
        case CALLER_WAYPOINT:
            if (!coord_eq(a->waypoint.coord, b->waypoint.coord)) {
                return false;
            }
            if (a->waypoint.bezier_len != b->waypoint.bezier_len) {
                return false;
            }
            for (size_t i = 0; i < a->waypoint.bezier_len; i++) {
                if (!coord_eq(a->waypoint.bezier[i], b->waypoint.bezier[i])) {
                    return false;
                }
            }
            if (a->waypoint.waypoint_handle && b->waypoint.waypoint_handle) {
                return a->waypoint.waypoint_handle == b->waypoint.waypoint_handle;
            }
            if (!a->waypoint.waypoint_id || !b->waypoint.waypoint_id) {
                return a->waypoint.waypoint_id == b->waypoint.waypoint_id;
            }
            return strcmp(a->waypoint.waypoint_id, b->waypoint.waypoint_id) == 0;
    }
    return false;
}

// Shallow copy with the id hashes filled in, so caller_key_matches can reject a
// non-matching entry with an integer compare instead of strcmp. The string
// pointers are borrowed, not owned.
void effect_callback_free(EffectCallback *cb) {
    for (size_t i = 0; i < cb->args_len; i++) {
        if (cb->args[i].kind == CALLBACK_STR) {
            free(cb->args[i].s);
        }
    }
    free(cb->args);
    cb->args = NULL;
    cb->args_len = 0;
}

void effect_callback_copy(EffectCallback *dst, const EffectCallback *src) {
    dst->id = src->id;
    dst->args_len = src->args_len;
    dst->args = NULL;
    if (src->args_len) {
        dst->args = malloc(src->args_len * sizeof(CallbackValue));
        memcpy(dst->args, src->args, src->args_len * sizeof(CallbackValue));
        for (size_t i = 0; i < src->args_len; i++) {
            if (src->args[i].kind == CALLBACK_STR) {
                dst->args[i].s = dup_cstr(src->args[i].s);
            }
        }
    }
}

void event_action_free(EventAction *action) {
    free(action->id);
    effect_callback_free(&action->cb);
    memset(action, 0, sizeof(*action));
}

void event_action_copy(EventAction *dst, const EventAction *src) {
    memset(dst, 0, sizeof(*dst));
    dst->kind = src->kind;
    dst->id = dup_cstr(src->id);
    dst->has_id = src->has_id;
    dst->layer = src->layer;
    dst->coord = src->coord;
    effect_callback_copy(&dst->cb, &src->cb);
}

void event_handler_init(EventHandler *h) {
    memset(h, 0, sizeof(*h));
}

void event_handler_free(EventHandler *h) {
    for (size_t i = 0; i < h->len; i++) {
        caller_key_free(&h->entries[i].caller);
        for (size_t j = 0; j < h->entries[i].actions_len; j++) {
            event_action_free(&h->entries[i].actions[j]);
        }
        free(h->entries[i].actions);
    }
    free(h->entries);
    memset(h, 0, sizeof(*h));
}

void event_handler_clear(EventHandler *h) {
    event_handler_free(h);
}

bool event_handler_subscribes(const EventHandler *h, Event event) {
    return (h->subscribed & (1u << (unsigned)event)) != 0;
}

static RegisteredEvent *find_entry(EventHandler *h, Event event, const CallerKey *caller) {
    CallerKey q = *caller;
    for (size_t i = 0; i < h->len; i++) {
        if (h->entries[i].event == event && caller_key_matches(&h->entries[i].caller, &q)) {
            return &h->entries[i];
        }
    }
    return NULL;
}

static bool action_equal(const EventAction *a, const EventAction *b) {
    if (a->kind != b->kind) {
        return false;
    }
    switch (a->kind) {
        case ACTION_ACTIVATE_PATH:
        case ACTION_ACTIVATE_SCENE:
            return a->id && b->id && strcmp(a->id, b->id) == 0;
        case ACTION_DEACTIVATE_PATH:
        case ACTION_DEACTIVATE_SCENE:
            if (a->has_id != b->has_id) return false;
            if (!a->has_id) return true;
            return a->id && b->id && strcmp(a->id, b->id) == 0;
        case ACTION_SET_LAYER:
            return a->layer == b->layer;
        case ACTION_SET_COORDINATE:
            return coord_eq(a->coord, b->coord);
        case ACTION_CALLBACK:
            return a->cb.id == b->cb.id && a->cb.args_len == b->cb.args_len;
        case ACTION_RESET_APPEARANCE:
            return true;
    }
    return false;
}

int event_handler_push(EventHandler *h, Event event, const CallerKey *caller, const EventAction *action) {
    RegisteredEvent *entry = find_entry(h, event, caller);
    if (entry) {
        for (size_t i = 0; i < entry->actions_len; i++) {
            if (action_equal(&entry->actions[i], action)) {
                return -1;
            }
        }
        if (entry->actions_len == entry->actions_cap) {
            size_t cap = entry->actions_cap ? entry->actions_cap * 2 : 4;
            EventAction *grown = realloc(entry->actions, cap * sizeof(EventAction));
            if (!grown) {
                return -1;
            }
            entry->actions = grown;
            entry->actions_cap = cap;
        }
        event_action_copy(&entry->actions[entry->actions_len++], action);
    } else {
        if (h->len == h->cap) {
            size_t cap = h->cap ? h->cap * 2 : 8;
            RegisteredEvent *grown = realloc(h->entries, cap * sizeof(RegisteredEvent));
            if (!grown) {
                return -1;
            }
            h->entries = grown;
            h->cap = cap;
        }
        RegisteredEvent *e = &h->entries[h->len++];
        memset(e, 0, sizeof(*e));
        e->event = event;
        caller_key_copy(&e->caller, caller);
        e->actions = malloc(4 * sizeof(EventAction));
        e->actions_cap = 4;
        e->actions_len = 0;
        event_action_copy(&e->actions[e->actions_len++], action);
    }
    h->subscribed |= (uint8_t)(1u << (unsigned)event);
    return 0;
}

long event_handler_actions_index(const EventHandler *h, Event event, const CallerKey *caller) {
    if (!event_handler_subscribes(h, event)) {
        return -1;
    }
    CallerKey q = *caller;
    for (size_t i = 0; i < h->len; i++) {
        if (h->entries[i].event == event && caller_key_matches(&h->entries[i].caller, &q)) {
            return (long)i;
        }
    }
    return -1;
}

const EventAction *event_handler_action(const EventHandler *h, size_t entry, size_t index) {
    if (entry >= h->len || index >= h->entries[entry].actions_len) {
        return NULL;
    }
    return &h->entries[entry].actions[index];
}

size_t event_handler_action_count(const EventHandler *h, size_t entry) {
    return entry < h->len ? h->entries[entry].actions_len : 0;
}
