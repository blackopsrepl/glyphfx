#include <string.h>

#include "engine/events.h"
#include "utils/strtab.h"
#include "testutil.h"

static void check_lookup(EventHandler *handler, Event event, const CallerKey *caller) {
    long indexed = event_handler_actions_index(handler, event, caller);
    long linear = -1;
    for (size_t i = 0; i < handler->len; i++) {
        if (handler->entries[i].event == event && caller_key_matches(&handler->entries[i].caller, caller)) {
            linear = (long)i;
            break;
        }
    }
    CHECK_EQ_INT(indexed, linear);
}

int main(void) {
    EventHandler handler;
    event_handler_init(&handler);
    for (unsigned i = 0; i < 96; i++) {
        char name[32];
        snprintf(name, sizeof(name), "caller-%u", i);
        CallerKey caller = {0};
        caller.kind = i % 2 ? CALLER_PATH : CALLER_SCENE;
        caller.id = name;
        caller.id_handle = strtab_handle(name);
        Event event = (Event)(i % 7);
        EventAction action = {0};
        action.kind = ACTION_SET_LAYER;
        action.layer = i;
        CHECK_EQ_INT(event_handler_push(&handler, event, &caller, &action), 0);
        CHECK_EQ_INT(event_handler_push(&handler, event, &caller, &action), -1);
        check_lookup(&handler, event, &caller);
        action.layer++;
        CHECK_EQ_INT(event_handler_push(&handler, event, &caller, &action), 0);
    }
    CHECK(handler.cap >= handler.len);
    for (size_t i = 0; i < handler.len; i++) {
        CallerKey caller = handler.entries[i].caller;
        for (unsigned event = 0; event < 7; event++) {
            check_lookup(&handler, (Event)event, &caller);
        }
        caller.id_handle = 0;
        CHECK_EQ_INT(event_handler_actions_index(&handler, handler.entries[i].event, &caller), (long)i);
        CHECK_EQ_INT(event_handler_action_count(&handler, i), 2);
    }
    Coord controls[] = {{1, 2}, {3, 4}};
    for (int64_t i = 0; i < 12; i++) {
        CallerKey caller = {0};
        caller.kind = CALLER_WAYPOINT;
        caller.waypoint.waypoint_id = "shared waypoint";
        caller.waypoint.waypoint_handle = strtab_handle(caller.waypoint.waypoint_id);
        caller.waypoint.coord = (Coord){i, i};
        caller.waypoint.bezier = controls;
        caller.waypoint.bezier_len = 2;
        EventAction action = {0};
        action.kind = ACTION_RESET_APPEARANCE;
        CHECK_EQ_INT(event_handler_push(&handler, EVENT_SEGMENT_ENTERED, &caller, &action), 0);
        check_lookup(&handler, EVENT_SEGMENT_ENTERED, &caller);
        caller.waypoint.coord.column += 100;
        CHECK_EQ_INT(event_handler_actions_index(&handler, EVENT_SEGMENT_ENTERED, &caller), -1);
    }
    CallerKey missing = {0};
    missing.kind = CALLER_PATH;
    missing.id = "unregistered";
    missing.id_handle = strtab_handle(missing.id);
    CHECK_EQ_INT(event_handler_actions_index(&handler, EVENT_PATH_COMPLETE, &missing), -1);
    event_handler_clear(&handler);
    CHECK_EQ_INT(handler.len, 0);
    CHECK_EQ_INT(handler.cap, 0);
    CHECK(handler.entries == NULL);
    CHECK_EQ_INT(event_handler_actions_index(&handler, EVENT_PATH_COMPLETE, &missing), -1);
    event_handler_free(&handler);
    return test_summary("test_events");
}
