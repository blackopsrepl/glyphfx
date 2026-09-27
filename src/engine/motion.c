#include "engine/motion.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/geometry.h"
#include "utils/pycompat.h"
#include "utils/strtab.h"

static char *dup_cstr(const char *s) {
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, s, len + 1);
    return out;
}

void waypoint_free(Waypoint *w) {
    // waypoint_id is interned and shared; the table owns it.
    free(w->bezier);
    memset(w, 0, sizeof(*w));
}

void waypoint_copy(Waypoint *dst, const Waypoint *src) {
    memset(dst, 0, sizeof(*dst));
    dst->waypoint_id = src->waypoint_id;
    dst->coord = src->coord;
    dst->has_bezier = src->has_bezier;
    if (src->has_bezier && src->bezier_len) {
        dst->bezier = malloc(src->bezier_len * sizeof(Coord));
        memcpy(dst->bezier, src->bezier, src->bezier_len * sizeof(Coord));
        dst->bezier_len = src->bezier_len;
    }
}

void segment_free(Segment *s) {
    waypoint_free(&s->start);
    waypoint_free(&s->end);
    memset(s, 0, sizeof(*s));
}

void segment_copy(Segment *dst, const Segment *src) {
    memset(dst, 0, sizeof(*dst));
    waypoint_copy(&dst->start, &src->start);
    waypoint_copy(&dst->end, &src->end);
    dst->distance = src->distance;
    dst->enter_event_triggered = src->enter_event_triggered;
    dst->exit_event_triggered = src->exit_event_triggered;
}

Path *path_new(const char *path_id, double speed, bool has_ease, Easing ease, bool has_layer, int64_t layer,
               int64_t hold_time, bool loop_) {
    if (speed <= 0.0) {
        return NULL;
    }
    Path *p = calloc(1, sizeof(Path));
    if (!p) {
        return NULL;
    }
    p->path_id = dup_cstr(path_id);
    p->speed = speed;
    p->has_ease = has_ease;
    p->ease = ease;
    p->has_layer = has_layer;
    p->layer = layer;
    p->hold_time = hold_time;
    p->hold_time_remaining = hold_time;
    p->loop_ = loop_;
    return p;
}

void path_free(Path *p) {
    if (!p) {
        return;
    }
    for (size_t i = 0; i < p->segments_len; i++) {
        segment_free(&p->segments[i]);
    }
    free(p->segments);
    for (size_t i = 0; i < p->waypoints_len; i++) {
        waypoint_free(&p->waypoints[i]);
    }
    free(p->waypoints);
    if (p->has_origin_segment) {
        segment_free(&p->origin_segment);
    }
    free(p->path_id);
    free(p);
}

static void path_push_waypoint(Path *p, Waypoint wp) {
    if (p->waypoints_len == p->waypoints_cap) {
        size_t cap = p->waypoints_cap ? p->waypoints_cap * 2 : 8;
        Waypoint *grown = realloc(p->waypoints, cap * sizeof(Waypoint));
        if (!grown) {
            return;
        }
        p->waypoints = grown;
        p->waypoints_cap = cap;
    }
    p->waypoints[p->waypoints_len++] = wp;
}

static void path_push_segment(Path *p, Segment seg) {
    if (p->segments_len == p->segments_cap) {
        size_t cap = p->segments_cap ? p->segments_cap * 2 : 8;
        Segment *grown = realloc(p->segments, cap * sizeof(Segment));
        if (!grown) {
            return;
        }
        p->segments = grown;
        p->segments_cap = cap;
    }
    p->segments[p->segments_len++] = seg;
}

static void add_waypoint_to_path(Path *p, Waypoint waypoint) {
    path_push_waypoint(p, waypoint);
    if (p->waypoints_len < 2) {
        return;
    }
    Waypoint *prev = &p->waypoints[p->waypoints_len - 2];
    Waypoint *wp = &p->waypoints[p->waypoints_len - 1];
    double distance;
    if (wp->has_bezier) {
        distance = find_length_of_bezier_curve(prev->coord, wp->bezier, wp->bezier_len, wp->coord);
    } else {
        distance = find_length_of_line(prev->coord, wp->coord, true);
    }
    p->total_distance += distance;
    Segment seg;
    memset(&seg, 0, sizeof(seg));
    waypoint_copy(&seg.start, prev);
    waypoint_copy(&seg.end, wp);
    seg.distance = distance;
    path_push_segment(p, seg);
    p->max_steps = py_round_half_even(p->total_distance / p->speed);
}

int path_new_waypoint(Path *p, Coord coord, const Coord *bezier, size_t bezier_len, const char *waypoint_id,
                      Waypoint *out) {
    Waypoint wp;
    memset(&wp, 0, sizeof(wp));
    if (waypoint_id[0] == '\0') {
        size_t current_id = p->waypoints_len;
        char candidate[32];
        for (;;) {
            snprintf(candidate, sizeof(candidate), "%zu", current_id);
            bool exists = false;
            for (size_t i = 0; i < p->waypoints_len; i++) {
                if (p->waypoints[i].waypoint_id && strcmp(p->waypoints[i].waypoint_id, candidate) == 0) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                break;
            }
            current_id++;
        }
        wp.waypoint_id = strtab_intern(candidate);
    } else {
        for (size_t i = 0; i < p->waypoints_len; i++) {
            if (p->waypoints[i].waypoint_id && strcmp(p->waypoints[i].waypoint_id, waypoint_id) == 0) {
                return -1;
            }
        }
        wp.waypoint_id = strtab_intern(waypoint_id);
    }
    wp.coord = coord;
    if (bezier && bezier_len > 0) {
        wp.has_bezier = true;
        wp.bezier = malloc(bezier_len * sizeof(Coord));
        memcpy(wp.bezier, bezier, bezier_len * sizeof(Coord));
        wp.bezier_len = bezier_len;
    }
    // Python: empty tuple bezier_control is falsy -> None.
    if (wp.has_bezier && wp.bezier_len == 0) {
        wp.has_bezier = false;
    }
    add_waypoint_to_path(p, wp);
    // The stored copy owns the strings; return a copy to the caller.
    waypoint_copy(out, &p->waypoints[p->waypoints_len - 1]);
    return 0;
}

const Waypoint *path_query_waypoint(const Path *p, const char *waypoint_id) {
    for (size_t i = 0; i < p->waypoints_len; i++) {
        if (p->waypoints[i].waypoint_id && strcmp(p->waypoints[i].waypoint_id, waypoint_id) == 0) {
            return &p->waypoints[i];
        }
    }
    return NULL;
}

void motion_init(Motion *m, Coord input_coord) {
    memset(m, 0, sizeof(*m));
    om_init(&m->paths);
    m->current_coord = input_coord;
    m->previous_coord = coord_new(-1, -1);
}

void motion_free(Motion *m) {
    for (size_t i = 0; i < om_len(&m->paths); i++) {
        path_free((Path *)om_value_at(&m->paths, i));
    }
    om_free(&m->paths);
    free(m->active_path);
    free(m->completed_path);
    memset(m, 0, sizeof(*m));
}

void motion_set_coordinate(Motion *m, Coord coord) {
    m->current_coord = coord;
}

int motion_new_path(Motion *m, double speed, bool has_ease, Easing ease, bool has_layer, int64_t layer,
                    int64_t hold_time, bool loop_, const char *path_id, char **out_id) {
    char auto_id[32];
    if (path_id[0] == '\0') {
        size_t current_id = om_len(&m->paths);
        for (;;) {
            snprintf(auto_id, sizeof(auto_id), "%zu", current_id);
            if (!om_contains(&m->paths, auto_id)) {
                break;
            }
            current_id++;
        }
        path_id = auto_id;
    } else if (om_contains(&m->paths, path_id)) {
        return -1;
    }
    Path *p = path_new(path_id, speed, has_ease, ease, has_layer, layer, hold_time, loop_);
    if (!p) {
        return -1;
    }
    om_insert(&m->paths, path_id, p);
    if (out_id) {
        *out_id = dup_cstr(path_id);
    }
    return 0;
}

bool motion_movement_is_complete(const Motion *m) {
    return m->active_path == NULL;
}

void motion_deactivate_path(Motion *m, const char *path_id) {
    if (!path_id) {
        free(m->active_path);
        m->active_path = NULL;
        m->active_path_slot_valid = false;
        return;
    }
    if (m->active_path && strcmp(m->active_path, path_id) == 0) {
        free(m->active_path);
        m->active_path = NULL;
        m->active_path_slot_valid = false;
    }
}

void motion_clear_paths(Motion *m) {
    for (size_t i = 0; i < om_len(&m->paths); i++) {
        path_free((Path *)om_value_at(&m->paths, i));
    }
    om_clear(&m->paths);
    free(m->active_path);
    m->active_path = NULL;
    m->active_path_slot_valid = false;
}
