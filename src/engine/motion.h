// Waypoint, Segment, Path, Motion, ported from the reference engine/motion.py.
// Stepping that fires events lives on EngineCtx.
#ifndef GLYPHFX_MOTION_H
#define GLYPHFX_MOTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "utils/easing.h"
#include "utils/geometry.h"
#include "utils/ordmap.h"

typedef struct {
    const char *waypoint_id;  // interned, shared, never freed per-waypoint
    Coord coord;
    bool has_bezier;
    Coord *bezier;  // owned
    size_t bezier_len;
} Waypoint;

typedef struct {
    Waypoint start;
    Waypoint end;
    double distance;
    bool enter_event_triggered;
    bool exit_event_triggered;
} Segment;

typedef struct {
    char *path_id;  // owned
    double speed;
    bool has_ease;
    Easing ease;
    bool has_layer;
    int64_t layer;
    int64_t hold_time;
    bool loop_;
    Segment *segments;
    size_t segments_len;
    size_t segments_cap;
    Waypoint *waypoints;
    size_t waypoints_len;
    size_t waypoints_cap;
    double total_distance;
    int64_t current_step;
    int64_t max_steps;
    int64_t hold_time_remaining;
    double last_distance_reached;
    bool has_origin_segment;
    Segment origin_segment;
} Path;

typedef struct {
    OrdMap paths;  // char* -> Path*
    Coord current_coord;
    Coord previous_coord;
    char *active_path;  // owned, or NULL
    size_t active_path_slot;  // cached OrdMap slot for the active path
    bool active_path_slot_valid;
    char *completed_path;  // owned, or NULL
} Motion;

void waypoint_free(Waypoint *w);
void waypoint_copy(Waypoint *dst, const Waypoint *src);
void segment_free(Segment *s);
void segment_copy(Segment *dst, const Segment *src);

Path *path_new(const char *path_id, double speed, bool has_ease, Easing ease, bool has_layer, int64_t layer,
               int64_t hold_time, bool loop_);
void path_free(Path *p);
// Returns 0; on duplicate explicit id returns -1.
int path_new_waypoint(Path *p, Coord coord, const Coord *bezier, size_t bezier_len, const char *waypoint_id,
                      Waypoint *out);
const Waypoint *path_query_waypoint(const Path *p, const char *waypoint_id);

void motion_init(Motion *m, Coord input_coord);
void motion_free(Motion *m);
void motion_set_coordinate(Motion *m, Coord coord);
// Returns 0 and sets *out_id (owned, caller frees) on success; -1 on duplicate.
int motion_new_path(Motion *m, double speed, bool has_ease, Easing ease, bool has_layer, int64_t layer,
                    int64_t hold_time, bool loop_, const char *path_id, char **out_id);
bool motion_movement_is_complete(const Motion *m);
void motion_deactivate_path(Motion *m, const char *path_id);
// Removes every path (used by particle reset).
void motion_clear_paths(Motion *m);

#endif
