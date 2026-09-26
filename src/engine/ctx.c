#include "engine/ctx.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/canvas.h"
#include "engine/motion.h"
#include "utils/pycompat.h"

int engine_ctx_init(EngineCtx *ctx, const char *input_data, const TerminalConfig *config, Rng rng, Clock clock,
                    PreprocessError *err) {
    memset(ctx, 0, sizeof(*ctx));
    if (terminal_new(&ctx->terminal, input_data, config, err) != 0) {
        return -1;
    }
    ctx->rng = rng;
    ctx->clock = clock;
    ac_init(&ctx->active_characters);
    ctx->preexisting_colors_present = false;
    for (size_t i = 0; i < ctx->terminal.input_characters_len; i++) {
        EffectCharacter *ch = &ctx->terminal.arena.items[ctx->terminal.input_characters[i]];
        if (ch->animation.has_input_fg || ch->animation.has_input_bg) {
            ctx->preexisting_colors_present = true;
            break;
        }
    }
    return 0;
}

void engine_ctx_free(EngineCtx *ctx) {
    terminal_free(&ctx->terminal);
    ac_free(&ctx->active_characters);
    free(ctx->scratch);
    memset(ctx, 0, sizeof(*ctx));
}

static bool observes_event(const EngineCtx *ctx, CharId id, Event event) {
    return event_handler_subscribes(&ctx->terminal.arena.items[id].event_handler, event);
}

static CallerKey scene_caller(const char *id) {
    CallerKey key;
    memset(&key, 0, sizeof(key));
    key.kind = CALLER_SCENE;
    key.id = (char *)id;
    return key;
}

static CallerKey path_caller(const char *id) {
    CallerKey key;
    memset(&key, 0, sizeof(key));
    key.kind = CALLER_PATH;
    key.id = (char *)id;
    return key;
}

void engine_caller_from_waypoint(const Waypoint *wp, CallerKey *out) {
    memset(out, 0, sizeof(*out));
    out->kind = CALLER_WAYPOINT;
    out->waypoint.coord = wp->coord;
    out->waypoint.waypoint_id = wp->waypoint_id ? malloc(strlen(wp->waypoint_id) + 1) : NULL;
    if (out->waypoint.waypoint_id) {
        strcpy(out->waypoint.waypoint_id, wp->waypoint_id);
    }
    if (wp->bezier_len) {
        out->waypoint.bezier = malloc(wp->bezier_len * sizeof(Coord));
        memcpy(out->waypoint.bezier, wp->bezier, wp->bezier_len * sizeof(Coord));
    }
    out->waypoint.bezier_len = wp->bezier_len;
}

// --- events ---------------------------------------------------------------

int engine_register_event(EngineCtx *ctx, CharId id, Event event, const CallerKey *caller, const EventAction *action) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    if (caller->kind == CALLER_PATH && !om_contains(&ch->motion.paths, caller->id)) {
        return -1;
    }
    if (caller->kind == CALLER_SCENE && !om_contains(&ch->animation.scenes, caller->id)) {
        return -1;
    }
    if ((action->kind == ACTION_ACTIVATE_PATH || action->kind == ACTION_DEACTIVATE_PATH) && action->has_id &&
        !om_contains(&ch->motion.paths, action->id)) {
        return -1;
    }
    if ((action->kind == ACTION_ACTIVATE_SCENE || action->kind == ACTION_DEACTIVATE_SCENE) && action->has_id &&
        !om_contains(&ch->animation.scenes, action->id)) {
        return -1;
    }
    return event_handler_push(&ch->event_handler, event, caller, action);
}

int engine_chain_paths(EngineCtx *ctx, CharId id, const char *const *paths, size_t n_paths, bool loop) {
    if (n_paths < 2) {
        return 0;
    }
    for (size_t i = 1; i < n_paths; i++) {
        EventAction action;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_PATH;
        action.id = (char *)paths[i];
        CallerKey caller = path_caller(paths[i - 1]);
        if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
            return -1;
        }
    }
    if (loop) {
        EventAction action;
        memset(&action, 0, sizeof(action));
        action.kind = ACTION_ACTIVATE_PATH;
        action.id = (char *)paths[0];
        CallerKey caller = path_caller(paths[n_paths - 1]);
        if (engine_register_event(ctx, id, EVENT_PATH_COMPLETE, &caller, &action) != 0) {
            return -1;
        }
    }
    return 0;
}

void engine_handle_event(EngineCtx *ctx, Effect *effect, CharId id, Event event, const CallerKey *caller) {
    for (size_t action_index = 0;; action_index++) {
        EventHandler *handler = &ctx->terminal.arena.items[id].event_handler;
        long entry = event_handler_actions_index(handler, event, caller);
        if (entry < 0) {
            return;
        }
        size_t count = event_handler_action_count(handler, (size_t)entry);
        if (action_index >= count) {
            return;
        }
        const EventAction *action = event_handler_action(handler, (size_t)entry, action_index);
        // Copy: a callback may append actions and reallocate the list.
        EventAction local;
        event_action_copy(&local, action);
        switch (local.kind) {
            case ACTION_ACTIVATE_SCENE:
                engine_activate_scene(ctx, effect, id, local.id);
                break;
            case ACTION_DEACTIVATE_SCENE:
                engine_deactivate_scene(ctx, id, local.has_id ? local.id : NULL);
                break;
            case ACTION_ACTIVATE_PATH:
                engine_activate_path(ctx, effect, id, local.id);
                break;
            case ACTION_DEACTIVATE_PATH:
                motion_deactivate_path(&ctx->terminal.arena.items[id].motion, local.has_id ? local.id : NULL);
                break;
            case ACTION_RESET_APPEARANCE: {
                EffectCharacter *ch = &ctx->terminal.arena.items[id];
                animation_set_appearance(&ch->animation, ch->uses_input_preexisting_colors, ch->input_symbol, NULL);
                break;
            }
            case ACTION_SET_LAYER:
                ctx->terminal.arena.items[id].layer = local.layer;
                break;
            case ACTION_SET_COORDINATE:
                motion_set_coordinate(&ctx->terminal.arena.items[id].motion, local.coord);
                break;
            case ACTION_CALLBACK:
                if (effect->ops->dispatch_callback) {
                    effect->ops->dispatch_callback(effect, ctx, id, &local.cb);
                }
                break;
        }
        event_action_free(&local);
    }
}

// --- motion ---------------------------------------------------------------

static void path_replace_segment(Path *p, size_t index, const Segment *seg) {
    segment_free(&p->segments[index]);
    segment_copy(&p->segments[index], seg);
}

static void path_insert_segment_front(Path *p, const Segment *seg) {
    if (p->segments_len == p->segments_cap) {
        size_t cap = p->segments_cap ? p->segments_cap * 2 : 8;
        Segment *grown = realloc(p->segments, cap * sizeof(Segment));
        if (!grown) {
            return;
        }
        p->segments = grown;
        p->segments_cap = cap;
    }
    memmove(p->segments + 1, p->segments, p->segments_len * sizeof(Segment));
    p->segments_len++;
    memset(&p->segments[0], 0, sizeof(Segment));
    segment_copy(&p->segments[0], seg);
}

void engine_activate_path(EngineCtx *ctx, Effect *effect, CharId id, const char *path_id) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    Path *p = (Path *)om_get(&ch->motion.paths, path_id);
    if (!p || p->waypoints_len == 0) {
        return;
    }
    Coord current = ch->motion.current_coord;
    const Waypoint *first = &p->waypoints[0];
    double distance_to_first = first->has_bezier
                                   ? find_length_of_bezier_curve(current, first->bezier, first->bezier_len,
                                                                 first->coord)
                                   : find_length_of_line(current, first->coord, true);

    Segment new_origin;
    memset(&new_origin, 0, sizeof(new_origin));
    new_origin.start.waypoint_id = malloc(7);
    strcpy(new_origin.start.waypoint_id, "origin");
    new_origin.start.coord = current;
    waypoint_copy(&new_origin.end, first);
    new_origin.distance = distance_to_first;

    free(ch->motion.active_path);
    ch->motion.active_path = malloc(strlen(path_id) + 1);
    strcpy(ch->motion.active_path, path_id);

    p->total_distance += distance_to_first;
    if (p->has_origin_segment) {
        p->total_distance -= p->origin_segment.distance;
        path_replace_segment(p, 0, &new_origin);
        // release the old origin's stored copy
        if (p->has_origin_segment) {
            segment_free(&p->origin_segment);
        }
    } else {
        path_insert_segment_front(p, &new_origin);
    }
    memset(&p->origin_segment, 0, sizeof(p->origin_segment));
    segment_copy(&p->origin_segment, &new_origin);
    p->has_origin_segment = true;
    p->current_step = 0;
    p->hold_time_remaining = p->hold_time;
    p->max_steps = py_round_half_even(p->total_distance / p->speed);
    for (size_t i = 0; i < p->segments_len; i++) {
        p->segments[i].enter_event_triggered = false;
        p->segments[i].exit_event_triggered = false;
    }
    if (p->has_layer) {
        ch->layer = p->layer;
    }
    segment_free(&new_origin);
    if (observes_event(ctx, id, EVENT_PATH_ACTIVATED)) {
        CallerKey caller = path_caller(path_id);
        engine_handle_event(ctx, effect, id, EVENT_PATH_ACTIVATED, &caller);
    }
}

static Coord path_step(EngineCtx *ctx, Effect *effect, CharId id, const char *path_id) {
    Path *p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, path_id);
    if (!p) {
        return ctx->terminal.arena.items[id].motion.current_coord;
    }
    if (p->max_steps == 0 || p->current_step >= p->max_steps || p->total_distance == 0.0) {
        return p->segments[p->segments_len - 1].end.coord;
    }
    p->current_step += 1;
    double ratio = (double)p->current_step / (double)p->max_steps;
    double distance_factor = p->has_ease ? easing_ease(&p->ease, ratio) : ratio;
    double distance_to_travel = distance_factor * p->total_distance;
    p->last_distance_reached = distance_to_travel;

    long active = -1;
    size_t i = 0;
    for (;;) {
        p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, path_id);
        if (!p || i >= p->segments_len) {
            break;
        }
        double seg_distance = p->segments[i].distance;
        bool enter_triggered = p->segments[i].enter_event_triggered;
        bool exit_triggered = p->segments[i].exit_event_triggered;
        if (distance_to_travel <= seg_distance) {
            active = (long)i;
            if (!enter_triggered) {
                if (observes_event(ctx, id, EVENT_SEGMENT_ENTERED)) {
                    CallerKey key;
                    engine_caller_from_waypoint(&p->segments[i].end, &key);
                    p->segments[i].enter_event_triggered = true;
                    engine_handle_event(ctx, effect, id, EVENT_SEGMENT_ENTERED, &key);
                    caller_key_free(&key);
                } else {
                    p->segments[i].enter_event_triggered = true;
                }
            }
            break;
        }
        distance_to_travel -= seg_distance;
        if (!enter_triggered || !exit_triggered) {
            bool observes = observes_event(ctx, id, EVENT_SEGMENT_ENTERED) ||
                            observes_event(ctx, id, EVENT_SEGMENT_EXITED);
            if (!observes) {
                p->segments[i].enter_event_triggered = true;
                p->segments[i].exit_event_triggered = true;
            } else {
                if (!enter_triggered) {
                    CallerKey key;
                    engine_caller_from_waypoint(&p->segments[i].end, &key);
                    p->segments[i].enter_event_triggered = true;
                    engine_handle_event(ctx, effect, id, EVENT_SEGMENT_ENTERED, &key);
                    caller_key_free(&key);
                }
                p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, path_id);
                if (!p) {
                    return ctx->terminal.arena.items[id].motion.current_coord;
                }
                if (!p->segments[i].exit_event_triggered) {
                    CallerKey key;
                    engine_caller_from_waypoint(&p->segments[i].end, &key);
                    p->segments[i].exit_event_triggered = true;
                    engine_handle_event(ctx, effect, id, EVENT_SEGMENT_EXITED, &key);
                    caller_key_free(&key);
                }
            }
        }
        i += 1;
    }
    p = (Path *)om_get(&ctx->terminal.arena.items[id].motion.paths, path_id);
    if (!p || p->segments_len == 0) {
        return ctx->terminal.arena.items[id].motion.current_coord;
    }
    if (active < 0) {
        active = (long)p->segments_len - 1;
        distance_to_travel += p->segments[active].distance;
    }
    Segment *seg = &p->segments[active];
    double t;
    if (seg->distance == 0.0) {
        t = 0.0;
    } else if (p->has_ease) {
        t = distance_to_travel / seg->distance;
    } else {
        double v = distance_to_travel / seg->distance;
        t = v < 1.0 ? v : 1.0;
    }
    if (seg->end.has_bezier) {
        return find_coord_on_bezier_curve(seg->start.coord, seg->end.bezier, seg->end.bezier_len, seg->end.coord, t);
    }
    return find_coord_on_line(seg->start.coord, seg->end.coord, t);
}

void engine_motion_move(EngineCtx *ctx, Effect *effect, CharId id) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    ch->motion.previous_coord = ch->motion.current_coord;
    if (!ch->motion.active_path) {
        return;
    }
    const char *active = ch->motion.active_path;
    Path *p = (Path *)om_get(&ch->motion.paths, active);
    if (!p || p->segments_len == 0) {
        return;
    }
    Coord new_coord = path_step(ctx, effect, id, active);
    ch = &ctx->terminal.arena.items[id];
    ch->motion.current_coord = new_coord;

    active = ch->motion.active_path;
    if (!active) {
        return;
    }
    p = (Path *)om_get(&ch->motion.paths, active);
    if (!p) {
        return;
    }
    if (p->current_step == p->max_steps) {
        if (p->hold_time != 0 && p->hold_time_remaining == p->hold_time) {
            if (observes_event(ctx, id, EVENT_PATH_HOLDING)) {
                CallerKey caller = path_caller(active);
                engine_handle_event(ctx, effect, id, EVENT_PATH_HOLDING, &caller);
            }
            p->hold_time_remaining -= 1;
            return;
        }
        if (p->hold_time_remaining != 0) {
            p->hold_time_remaining -= 1;
            return;
        }
        if (p->loop_ && p->segments_len > 1) {
            motion_deactivate_path(&ch->motion, active);
            engine_activate_path(ctx, effect, id, active);
        } else {
            char *completed = malloc(strlen(active) + 1);
            strcpy(completed, active);
            free(ch->motion.completed_path);
            ch->motion.completed_path = completed;
            motion_deactivate_path(&ch->motion, active);
            if (observes_event(ctx, id, EVENT_PATH_COMPLETE)) {
                CallerKey caller = path_caller(active);
                engine_handle_event(ctx, effect, id, EVENT_PATH_COMPLETE, &caller);
            }
        }
    }
}

// --- animation ------------------------------------------------------------

void engine_activate_scene(EngineCtx *ctx, Effect *effect, CharId id, const char *scene_id) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    Scene *scene = (Scene *)om_get(&ch->animation.scenes, scene_id);
    if (!scene) {
        return;
    }
    CharacterVisual tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (scene_activate(scene, &tmp) != 0) {
        return;
    }
    free(ch->animation.active_scene);
    ch->animation.active_scene = malloc(strlen(scene_id) + 1);
    strcpy(ch->animation.active_scene, scene_id);
    ch->animation.active_scene_current_step = 0;
    vis_move(&ch->animation.current_visual, &tmp);
    if (observes_event(ctx, id, EVENT_SCENE_ACTIVATED)) {
        CallerKey key = scene_caller(scene_id);
        engine_handle_event(ctx, effect, id, EVENT_SCENE_ACTIVATED, &key);
    }
}

void engine_deactivate_scene(EngineCtx *ctx, CharId id, const char *scene_id) {
    Animation *anim = &ctx->terminal.arena.items[id].animation;
    if (!scene_id) {
        free(anim->active_scene);
        anim->active_scene = NULL;
        return;
    }
    if (anim->active_scene && strcmp(anim->active_scene, scene_id) == 0) {
        free(anim->active_scene);
        anim->active_scene = NULL;
    }
}

static void complete_scene_if_finished(EngineCtx *ctx, Effect *effect, CharId id, Scene *scene) {
    if (!(iq_len(&scene->frames) == 0 || scene->is_looping)) {
        return;
    }
    Animation *anim = &ctx->terminal.arena.items[id].animation;
    if (!scene->is_looping) {
        scene_reset(scene);
        free(anim->active_scene);
        anim->active_scene = NULL;
    }
    if (observes_event(ctx, id, EVENT_SCENE_COMPLETE)) {
        CallerKey key = scene_caller(scene->scene_id);
        engine_handle_event(ctx, effect, id, EVENT_SCENE_COMPLETE, &key);
    }
}

static void step_synced_scene(EngineCtx *ctx, CharId id, Scene *scene, SyncMetric sync) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    bool has_path_state = false;
    int64_t current_step = 0;
    int64_t max_steps = 0;
    double total_distance = 0.0;
    double last_distance_reached = 0.0;
    if (ch->motion.active_path) {
        Path *p = (Path *)om_get(&ch->motion.paths, ch->motion.active_path);
        if (p) {
            has_path_state = true;
            current_step = p->current_step;
            max_steps = p->max_steps;
            total_distance = p->total_distance;
            last_distance_reached = p->last_distance_reached;
        }
    }
    if (!has_path_state) {
        size_t last;
        if (iq_back(&scene->frames, &last)) {
            vis_copy(&ch->animation.current_visual, &scene->all_frames[last].visual);
        }
        iq_append(&scene->played_frames, &scene->frames);
        return;
    }
    int64_t final_frame_index = (int64_t)iq_len(&scene->frames) - 1;
    double progress_ratio;
    if (sync == SYNC_STEP) {
        int64_t cs = current_step > 1 ? current_step : 1;
        int64_t ms = max_steps > 1 ? max_steps : 1;
        progress_ratio = (double)cs / (double)ms;
    } else {
        double total = total_distance > 1.0 ? total_distance : 1.0;
        double remaining = total_distance - last_distance_reached;
        if (remaining < 1.0) remaining = 1.0;
        double reached = total - remaining;
        if (reached < 1.0) reached = 1.0;
        progress_ratio = reached / total;
    }
    int64_t frame_index = py_round_half_even((double)final_frame_index * progress_ratio);
    if (frame_index > final_frame_index) frame_index = final_frame_index;
    if (frame_index < 0) frame_index = 0;
    size_t pos;
    if (iq_at(&scene->frames, (size_t)frame_index, &pos)) {
        vis_copy(&ch->animation.current_visual, &scene->all_frames[pos].visual);
    }
}

static void step_eased_scene(EngineCtx *ctx, CharId id, Scene *scene) {
    EffectCharacter *ch = &ctx->terminal.arena.items[id];
    double elapsed_step_ratio = (double)scene->easing_current_step / (double)scene->easing_total_steps;
    double easing_factor = easing_ease(&scene->ease, elapsed_step_ratio);
    int64_t final_frame_index = (scene->easing_total_steps - 1) > 0 ? (scene->easing_total_steps - 1) : 0;
    int64_t frame_index = py_round_half_even(easing_factor * (double)final_frame_index);
    if (frame_index > final_frame_index) frame_index = final_frame_index;
    if (frame_index < 0) frame_index = 0;
    if ((size_t)frame_index < scene->frame_index_map_len) {
        size_t frame = scene->frame_index_map[frame_index];
        vis_copy(&ch->animation.current_visual, &scene->all_frames[frame].visual);
    }
    scene->easing_current_step += 1;
    if (scene->easing_current_step == scene->easing_total_steps) {
        if (scene->is_looping) {
            scene->easing_current_step = 0;
        } else {
            iq_append(&scene->played_frames, &scene->frames);
        }
    }
}

void engine_step_animation(EngineCtx *ctx, Effect *effect, CharId id) {
    Animation *anim = &ctx->terminal.arena.items[id].animation;
    if (!anim->active_scene) {
        return;
    }
    Scene *scene = (Scene *)om_get(&anim->scenes, anim->active_scene);
    if (!scene || iq_len(&scene->frames) == 0) {
        return;
    }
    if (scene->has_sync) {
        step_synced_scene(ctx, id, scene, scene->sync);
    } else if (scene->has_ease) {
        step_eased_scene(ctx, id, scene);
    } else {
        CharacterVisual tmp;
        memset(&tmp, 0, sizeof(tmp));
        scene_get_next_visual(scene, &tmp);
        vis_move(&anim->current_visual, &tmp);
    }
    complete_scene_if_finished(ctx, effect, id, scene);
}

void engine_tick(EngineCtx *ctx, Effect *effect, CharId id) {
    engine_motion_move(ctx, effect, id);
    engine_step_animation(ctx, effect, id);
}

static bool retain_keep(CharId id, void *ctxp) {
    EngineCtx *ctx = ctxp;
    return character_is_active(&ctx->terminal.arena.items[id]);
}

void engine_update(EngineCtx *ctx, Effect *effect) {
    CharId *snapshot = NULL;
    size_t snapshot_len = 0;
    ac_snapshot(&ctx->active_characters, &snapshot, &snapshot_len);
    for (size_t i = 0; i < snapshot_len; i++) {
        engine_tick(ctx, effect, snapshot[i]);
    }
    free(snapshot);
    ac_retain(&ctx->active_characters, retain_keep, ctx);
}

char *engine_frame(EngineCtx *ctx) {
    if (clock_is_real(&ctx->clock) && ctx->terminal.config.frame_rate != 0) {
        terminal_enforce_framerate(&ctx->terminal);
    }
    clock_advance_frame(&ctx->clock);
    return terminal_get_formatted_output_string(&ctx->terminal);
}
