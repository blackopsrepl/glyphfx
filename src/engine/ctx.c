#include "engine/ctx.h"

#include <stdlib.h>
#include <string.h>

#include "engine/canvas.h"

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

static CallerKey caller_scene(const char *id) {
    CallerKey key;
    memset(&key, 0, sizeof(key));
    key.kind = CALLER_SCENE;
    key.id = (char *)id;
    return key;
}

static CallerKey caller_path(const char *id) {
    CallerKey key;
    memset(&key, 0, sizeof(key));
    key.kind = CALLER_PATH;
    key.id = (char *)id;
    return key;
}

// Path events arrive with the movement effects (M3).
static void caller_path_unused(void) __attribute__((unused));
static void caller_path_unused(void) {
    (void)caller_path;
}

void engine_handle_event(EngineCtx *ctx, Effect *effect, CharId id, Event event, const CallerKey *caller) {
    EventHandler *handler = &ctx->terminal.arena.items[id].event_handler;
    long entry = event_handler_actions_index(handler, event, caller);
    if (entry < 0) {
        return;
    }
    size_t action_index = 0;
    for (;;) {
        size_t count = event_handler_action_count(handler, (size_t)entry);
        if (action_index >= count) {
            break;
        }
        const EventAction *action = event_handler_action(handler, (size_t)entry, action_index);
        switch (action->kind) {
            case ACTION_ACTIVATE_SCENE:
                engine_activate_scene(ctx, effect, id, action->id);
                break;
            case ACTION_DEACTIVATE_SCENE:
                engine_deactivate_scene(ctx, id, action->has_id ? action->id : NULL);
                break;
            case ACTION_RESET_APPEARANCE: {
                EffectCharacter *ch = &ctx->terminal.arena.items[id];
                animation_set_appearance(&ch->animation, ch->uses_input_preexisting_colors, ch->input_symbol, NULL);
                break;
            }
            case ACTION_SET_LAYER:
                ctx->terminal.arena.items[id].layer = action->layer;
                break;
            case ACTION_SET_COORDINATE:
                ctx->terminal.arena.items[id].motion_coord = action->coord;
                break;
            case ACTION_CALLBACK:
                if (effect->ops->dispatch_callback) {
                    effect->ops->dispatch_callback(effect, ctx, id, &action->cb);
                }
                break;
            case ACTION_ACTIVATE_PATH:
            case ACTION_DEACTIVATE_PATH:
                // Path motion lands with the movement effects (M3).
                break;
        }
        action_index++;
    }
}

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
    if (ch->animation.active_scene) {
        strcpy(ch->animation.active_scene, scene_id);
    }
    ch->animation.active_scene_current_step = 0;
    vis_move(&ch->animation.current_visual, &tmp);
    if (observes_event(ctx, id, EVENT_SCENE_ACTIVATED)) {
        CallerKey key = caller_scene(scene_id);
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
        const char *scene_id = scene->scene_id;
        CallerKey key = caller_scene(scene_id);
        engine_handle_event(ctx, effect, id, EVENT_SCENE_COMPLETE, &key);
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
    // Sync/eased scenes arrive with the effects that use them; fall back to the
    // normal frame walk until then.
    CharacterVisual tmp;
    memset(&tmp, 0, sizeof(tmp));
    scene_get_next_visual(scene, &tmp);
    vis_move(&anim->current_visual, &tmp);
    complete_scene_if_finished(ctx, effect, id, scene);
}

void engine_motion_move(EngineCtx *ctx, Effect *effect, CharId id) {
    (void)ctx;
    (void)effect;
    (void)id;
    // Path stepping lands with the movement effects (M3).
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
