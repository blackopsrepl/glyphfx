#include <string.h>

#include "engine/ctx.h"
#include "engine/render_log.h"
#include "testutil.h"

typedef struct {
    CharId earlier, later;
    Path *first, *last;
    bool recurse;
    unsigned callbacks;
} UpdateProbe;

static void observe_order(Effect *effect, EngineCtx *ctx, CharId id, const EffectCallback *cb) {
    (void)id;
    (void)cb;
    UpdateProbe *probe = effect->state;
    probe->callbacks++;
    CHECK_EQ_INT(probe->first->current_step, 2);
    CHECK_EQ_INT(probe->last->current_step, 1);
    Motion *first = &ctx->terminal.arena.items[probe->earlier].motion;
    CHECK(coord_eq(first->previous_coord, first->current_coord));
    engine_activate_scene(ctx, effect, probe->later, "later");
    if (probe->recurse) engine_tick(ctx, effect, probe->later);
}

static const EffectOps OPS = {.dispatch_callback = observe_order};

static Path *slow_path(EngineCtx *ctx, Effect *effect, CharId id) {
    Motion *motion = &ctx->terminal.arena.items[id].motion;
    CHECK_EQ_INT(motion_new_path(motion, 0.1, false, (Easing){0}, false, 0, 0, false,
                                 "slow", NULL), 0);
    Path *path = om_get(&motion->paths, "slow");
    Coord target = motion->current_coord;
    target.column += 10;
    Waypoint waypoint;
    CHECK_EQ_INT(path_new_waypoint(path, target, NULL, 0, "", &waypoint), 0);
    waypoint_free(&waypoint);
    engine_activate_path(ctx, effect, id, "slow");
    CHECK(ac_insert(&ctx->active_characters, id));
    return path;
}

static void add_scene(EngineCtx *ctx, CharId id, const char *name, int64_t duration) {
    Animation *anim = &ctx->terminal.arena.items[id].animation;
    CHECK(animation_new_scene(anim, false, false, SYNC_STEP, false, (Easing){0}, name, false));
    Scene *scene = om_get(&anim->scenes, name);
    VisualParams params = {0};
    CHECK_EQ_INT(scene_add_frame(scene, "X", duration, &params), 0);
}

static void callback_order(bool recurse) {
    EngineCtx ctx;
    TerminalConfig config;
    terminal_config_default(&config);
    config.canvas_width = 3;
    config.canvas_height = 1;
    config.ignore_terminal_dimensions = true;
    PreprocessError err;
    CHECK_EQ_INT(engine_ctx_init(&ctx, "abc", &config, rng_seeded(1),
                                 clock_virtual_with_frame_rate(30), &err), 0);
    engine_bind(&ctx);
    UpdateProbe probe = {.recurse = recurse};
    Effect effect = {.ops = &OPS, .state = &probe};
    probe.earlier = ctx.terminal.input_characters[0];
    CharId middle = ctx.terminal.input_characters[1];
    probe.later = ctx.terminal.input_characters[2];
    CHECK(probe.earlier < middle && middle < probe.later);
    CHECK_EQ_INT(probe.earlier >> 6, probe.later >> 6);
    probe.first = slow_path(&ctx, &effect, probe.earlier);
    probe.last = slow_path(&ctx, &effect, probe.later);
    add_scene(&ctx, middle, "middle", 2);
    add_scene(&ctx, probe.later, "later", 5);
    engine_activate_scene(&ctx, &effect, middle, "middle");
    CHECK(ac_insert(&ctx.active_characters, middle));
    CallerKey caller = {.kind = CALLER_SCENE, .id = "middle"};
    EventAction action = {.kind = ACTION_CALLBACK};
    CHECK_EQ_INT(engine_register_event(&ctx, middle, EVENT_SCENE_COMPLETE, &caller, &action), 0);

    // First tick captures the paths; the next word has idle lanes around the
    // completing scene. The callback must not see the later idle lane advanced.
    engine_update(&ctx, &effect);
    CHECK_EQ_INT(probe.first->current_step, 1);
    CHECK_EQ_INT(probe.last->current_step, 1);
    CHECK(ctx.motion_mirrored[(size_t)probe.earlier >> 6] &
          (UINT64_C(1) << (probe.earlier & 63)));
    engine_update(&ctx, &effect);
    CHECK(ctx.motion_batch.idle & (UINT64_C(1) << (probe.earlier & 63)));
    CHECK(ctx.motion_batch.idle & (UINT64_C(1) << (probe.later & 63)));
    CHECK_EQ_INT(probe.callbacks, 1);
    CHECK_EQ_INT(probe.first->current_step, 2);
    CHECK_EQ_INT(probe.last->current_step, recurse ? 3 : 2);
    Scene *later = animation_active_scene(&ctx.terminal.arena.items[probe.later].animation);
    CHECK(later != NULL);
    CHECK_EQ_INT(later->ticks_elapsed, recurse ? 2 : 1);
    CHECK_EQ_INT(ctx.motion_batch.ready, 0);
    render_log_bind(NULL);
    engine_ctx_free(&ctx);
    engine_bind(NULL);
}

static void compare_scalar_ticks(void) {
    enum { SLOTS = 130 };
    char input[SLOTS + 1];
    memset(input, 'x', SLOTS);
    input[SLOTS] = '\0';
    EngineCtx batch, scalar;
    TerminalConfig config;
    terminal_config_default(&config);
    config.canvas_width = SLOTS;
    config.canvas_height = 1;
    config.ignore_terminal_dimensions = true;
    PreprocessError err;
    CHECK_EQ_INT(engine_ctx_init(&batch, input, &config, rng_seeded(1),
                                 clock_virtual_with_frame_rate(30), &err), 0);
    CHECK_EQ_INT(engine_ctx_init(&scalar, input, &config, rng_seeded(1),
                                 clock_virtual_with_frame_rate(30), &err), 0);
    Effect effect = {.ops = &OPS};
    Path *bp[SLOTS], *sp[SLOTS];
    for (size_t i = 0; i < SLOTS; i++) {
        engine_bind(&batch);
        bp[i] = slow_path(&batch, &effect, batch.terminal.input_characters[i]);
        engine_bind(&scalar);
        sp[i] = slow_path(&scalar, &effect, scalar.terminal.input_characters[i]);
    }
    // Whole idle words, bit 63, a partial final word, coordinate transitions,
    // and final path retirement all agree with the public scalar tick path.
    for (unsigned tick = 0; tick < 110; tick++) {
        engine_update(&batch, &effect);
        engine_bind(&scalar);
        for (size_t i = 0; i < SLOTS; i++) {
            CharId id = scalar.terminal.input_characters[i];
            engine_tick(&scalar, &effect, id);
            Motion *bm = &batch.terminal.arena.items[batch.terminal.input_characters[i]].motion;
            Motion *sm = &scalar.terminal.arena.items[id].motion;
            CHECK(coord_eq(bm->current_coord, sm->current_coord));
            CHECK(coord_eq(bm->previous_coord, sm->previous_coord));
            CHECK_EQ_INT(bp[i]->current_step, sp[i]->current_step);
            CHECK(bp[i]->last_distance_reached == sp[i]->last_distance_reached);
            CHECK((bm->active_path != NULL) == (sm->active_path != NULL));
        }
    }
    CHECK(ac_is_empty(&batch.active_characters));
    render_log_bind(NULL);
    engine_ctx_free(&batch);
    engine_ctx_free(&scalar);
    engine_bind(NULL);
}

int main(void) {
    callback_order(false);
    callback_order(true);
    compare_scalar_ticks();
    return test_summary("test_update");
}
