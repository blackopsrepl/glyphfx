// Event system, ported from the reference engine/base_character.py
// EventHandler. Storage is plain data; dispatch lives on EngineCtx so actions
// execute inline at the exact emission points, reentrantly.
#ifndef GLYPHFX_EVENTS_H
#define GLYPHFX_EVENTS_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "engine/charid.h"
#include "utils/geometry.h"
#include "utils/graphics.h"

typedef enum {
    EVENT_SEGMENT_ENTERED,
    EVENT_SEGMENT_EXITED,
    EVENT_PATH_ACTIVATED,
    EVENT_PATH_COMPLETE,
    EVENT_PATH_HOLDING,
    EVENT_SCENE_ACTIVATED,
    EVENT_SCENE_COMPLETE,
} Event;

typedef enum {
    CALLER_SCENE,
    CALLER_PATH,
    CALLER_WAYPOINT,
} CallerKind;

typedef struct {
    Coord coord;
    char *waypoint_id;  // owned
    Coord *bezier;      // owned, may be NULL
    size_t bezier_len;
} WaypointKey;

typedef struct {
    CallerKind kind;
    char *id;  // owned; scene or path id (NULL for waypoint)
    WaypointKey waypoint;
} CallerKey;

typedef enum {
    CALLBACK_INT,
    CALLBACK_FLOAT,
    CALLBACK_STR,
    CALLBACK_COORD,
    CALLBACK_CHAR,
    CALLBACK_COLOR,
} CallbackValueKind;

typedef struct {
    CallbackValueKind kind;
    int64_t i;
    double f;
    char *s;  // owned for CALLBACK_STR
    Coord coord;
    CharId character;
    Color color;
} CallbackValue;

typedef struct {
    uint32_t id;
    CallbackValue *args;
    size_t args_len;
} EffectCallback;

typedef enum {
    ACTION_ACTIVATE_PATH,
    ACTION_ACTIVATE_SCENE,
    ACTION_DEACTIVATE_PATH,
    ACTION_DEACTIVATE_SCENE,
    ACTION_RESET_APPEARANCE,
    ACTION_SET_LAYER,
    ACTION_SET_COORDINATE,
    ACTION_CALLBACK,
} ActionKind;

typedef struct {
    ActionKind kind;
    char *id;        // owned; path/scene id for activate/deactivate
    bool has_id;     // for deactivate with no target
    int64_t layer;
    Coord coord;
    EffectCallback cb;
} EventAction;

typedef struct {
    Event event;
    CallerKey caller;
    EventAction *actions;
    size_t actions_len;
    size_t actions_cap;
} RegisteredEvent;

typedef struct {
    RegisteredEvent *entries;
    size_t len;
    size_t cap;
    uint8_t subscribed;
} EventHandler;

void event_handler_init(EventHandler *h);
void event_handler_free(EventHandler *h);
void event_handler_clear(EventHandler *h);
bool event_handler_subscribes(const EventHandler *h, Event event);
// Returns 0 on success, -1 on duplicate registration.
int event_handler_push(EventHandler *h, Event event, const CallerKey *caller, const EventAction *action);
long event_handler_actions_index(const EventHandler *h, Event event, const CallerKey *caller);
const EventAction *event_handler_action(const EventHandler *h, size_t entry, size_t index);
size_t event_handler_action_count(const EventHandler *h, size_t entry);

void caller_key_free(CallerKey *key);
void caller_key_copy(CallerKey *dst, const CallerKey *src);
bool caller_key_matches(const CallerKey *a, const CallerKey *b);
void event_action_free(EventAction *action);
void event_action_copy(EventAction *dst, const EventAction *src);
void effect_callback_free(EffectCallback *cb);
void effect_callback_copy(EffectCallback *dst, const EffectCallback *src);

#endif
