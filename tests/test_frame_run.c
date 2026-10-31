#include <string.h>

#include "effects/colorshift.h"
#include "engine/ctx.h"
#include "testutil.h"

static Scene *new_scene(Animation *anim, const char *name) {
    CHECK(animation_new_scene(anim, false, false, SYNC_STEP, false, (Easing){0}, name, false));
    return om_get(&anim->scenes, name);
}

static void check_views(void) {
    Animation source, destination;
    animation_init(&source, "A");
    animation_init(&destination, "B");
    Scene *src = new_scene(&source, "source");
    Scene *dst = new_scene(&destination, "destination");
    const char *symbols[] = {"A", "B", "C"};
    VisualParams params = {0};
    for (size_t i = 0; i < 3; i++) {
        CHECK_EQ_INT(scene_add_frame(src, symbols[i], (int64_t)i + 2, &params), 0);
    }
    CHECK_EQ_INT(scene_append_frames(src, src), 0);
    FrameRun doubled = scene_frame_run(src);
    CHECK_EQ_INT(doubled.len, 6);
    CHECK_EQ_INT(doubled.total_steps, 18);
    FrameMemo *memo = framemo_new();
    CHECK(memo != NULL);
    Color key = {0};
    CHECK_EQ_INT(framemo_put(memo, "cycle", key, src), 0);

    FrameRun view;
    CHECK_EQ_INT(frame_run_slice(doubled, 1, 3, &view), 0);
    CHECK(view.frames == doubled.frames + 1);
    CHECK_EQ_INT(view.total_steps, 9);
    CHECK_EQ_INT(scene_set_frame_run(src, view), 0);
    CHECK_EQ_INT(scene_set_frame_run(dst, view), 0);
    CHECK(src->all_frames == dst->all_frames);
    const FrameRun *cached = framemo_get_run(memo, "cycle", key);
    CHECK(cached != NULL && cached->frames == doubled.frames);
    CHECK_EQ_INT(cached->len, 6);
    CHECK_EQ_INT(cached->total_steps, 18);
    VisualHandle shown;
    size_t frame;
    scene_get_next_visual(src, &shown, &frame);
    CHECK_EQ_INT(src->ticks_elapsed, 1);
    CHECK_EQ_INT(dst->ticks_elapsed, 0);
    CHECK_EQ_INT(scene_set_frame_run(src, view), 0);
    CHECK_EQ_INT(src->ticks_elapsed, 0);
    CHECK_EQ_INT(scene_add_frame(src, "D", 5, &params), 0);
    CHECK_EQ_INT(src->all_frames_len, 4);
    CHECK_EQ_INT(dst->all_frames_len, 3);
    CHECK_EQ_INT(dst->cold->easing_total_steps, 9);
    CHECK_EQ_INT(doubled.frames[4].visual, view.frames[0].visual);

    // Rehashing and source replacement do not change an inserted run snapshot.
    for (unsigned i = 0; i < 100; i++) {
        CHECK_EQ_INT(framemo_put(memo, "other", color_from_xterm((uint8_t)i), src), 0);
    }
    cached = framemo_get_run(memo, "cycle", key);
    CHECK(cached != NULL && cached->frames == doubled.frames);
    CHECK_EQ_INT(cached->len, 6);
    CHECK_EQ_INT(cached->total_steps, 18);
    animation_free(&source);
    CHECK_EQ_INT(frame_run_slice(*cached, 3, 3, &view), 0);
    CHECK_EQ_INT(scene_set_frame_run(dst, view), 0);
    for (size_t i = 0; i < 3; i++) {
        for (int64_t tick = 0; tick < (int64_t)i + 2; tick++) {
            scene_get_next_visual(dst, &shown, &frame);
            CHECK(strcmp(visual_symbol(shown), symbols[i]) == 0);
        }
    }
    CHECK_EQ_INT(dst->head, dst->all_frames_len);
    CHECK_EQ_INT(frame_run_slice(doubled, doubled.len, 0, &view), 0);
    CHECK_EQ_INT(view.len, 0);
    CHECK_EQ_INT(view.total_steps, 0);
    CHECK_EQ_INT(frame_run_slice(doubled, doubled.len + 1, 0, &view), -1);
    CHECK_EQ_INT(frame_run_slice(doubled, 1, SIZE_MAX, &view), -1);
    Frame overflow[] = {{.duration = INT64_MAX}, {.duration = 1}};
    CHECK_EQ_INT(frame_run_slice((FrameRun){overflow, 2, 0}, 0, 2, &view), -1);
    CHECK_EQ_INT(scene_set_frame_run(dst, (FrameRun){NULL, 1, 1}), -1);
    CHECK_EQ_INT(scene_set_frame_run(dst, (FrameRun){NULL, 0, 1}), -1);
    CHECK_EQ_INT(scene_set_frame_run(dst, (FrameRun){NULL, 0, -1}), -1);
    CHECK_EQ_INT(scene_set_frame_run(dst, (FrameRun){0}), 0);
    CHECK_EQ_INT(dst->head, 0);
    CHECK_EQ_INT(dst->all_frames_len, 0);
    CHECK_EQ_INT(dst->cold->easing_total_steps, 0);
    framemo_free(memo);
    animation_free(&destination);
}

static void check_colorshift_sharing(void) {
    EngineCtx ctx;
    TerminalConfig term;
    terminal_config_default(&term);
    term.canvas_width = 3;
    term.canvas_height = 1;
    term.ignore_terminal_dimensions = true;
    PreprocessError err;
    CHECK_EQ_INT(engine_ctx_init(&ctx, "aaa", &term, rng_seeded(1),
                                 clock_virtual_with_frame_rate(30), &err), 0);
    engine_bind(&ctx);
    ColorShiftConfig cfg;
    colorshift_config_defaults(&cfg);
    cfg.no_travel = true;
    Effect *effect = colorshift_make(&cfg);
    CHECK(effect != NULL);
    CHECK_EQ_INT(effect->ops->build(effect, &ctx), 0);
    Scene *scenes[3];
    for (size_t i = 0; i < 3; i++) {
        Animation *anim = &ctx.terminal.arena.items[ctx.terminal.input_characters[i]].animation;
        scenes[i] = animation_active_scene(anim);
        CHECK(scenes[i] != NULL);
        CHECK_EQ_INT(scenes[i]->head, 0);
        CHECK_EQ_INT(scenes[i]->ticks_elapsed, 0);
    }
    CHECK(scenes[0] != scenes[1] && scenes[1] != scenes[2]);
    CHECK(scenes[0]->all_frames == scenes[1]->all_frames);
    CHECK(scenes[1]->all_frames == scenes[2]->all_frames);
    VisualHandle shown;
    size_t frame;
    scene_get_next_visual(scenes[0], &shown, &frame);
    CHECK_EQ_INT(scenes[0]->ticks_elapsed, 1);
    CHECK_EQ_INT(scenes[1]->ticks_elapsed, 0);
    effect->ops->destroy(effect);
    colorshift_free_config(&cfg);
    engine_ctx_free(&ctx);
    engine_bind(NULL);
}

int main(void) {
    check_views();
    check_colorshift_sharing();
    return test_summary("test_frame_run");
}
