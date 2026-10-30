// Private ABI. Rust serializes ALL entry points, including creation/destruction.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "effects/registry.h"
#include "engine/ctx.h"
#include "engine/render_log.h"

typedef struct {
    EngineCtx ctx;
    Effect *effect;
    const EffectEntry *entry;
    void *config;
    RenderLog log;
    bool initialized;
} GlyphfxClip;

static void bind(GlyphfxClip *clip) {
    g_rlog = clip->log;
    render_log_bind(&clip->ctx.terminal);
}

static void detach(GlyphfxClip *clip) {
    clip->log = g_rlog;
    memset(&g_rlog, 0, sizeof(g_rlog));
    render_log_bind(NULL);
}

void glyphfx_close(GlyphfxClip *clip) {
    if (!clip) return;
    bind(clip);
    if (clip->effect) clip->effect->ops->destroy(clip->effect);
    if (clip->initialized) engine_ctx_free(&clip->ctx);
    if (clip->config) {
        clip->entry->free_config(clip->config);
        free(clip->config);
    }
    detach(clip);
    free(clip->log.buf);
    free(clip);
}

GlyphfxClip *glyphfx_open(const char *name, const char *text,
                         unsigned short width, unsigned short height,
                         unsigned long long seed, unsigned char anchor,
                         char *error, size_t error_len) {
    if (anchor > ANCHOR_C) {
        snprintf(error, error_len, "invalid text anchor: %u", anchor);
        return NULL;
    }
    const EffectEntry *entry = effect_find(name);
    if (!entry) {
        snprintf(error, error_len, "unknown effect: %s", name);
        return NULL;
    }
    GlyphfxClip *clip = calloc(1, sizeof(*clip));
    if (!clip) return NULL;
    clip->entry = entry;
    clip->config = calloc(1, entry->config_size);
    if (!clip->config) { free(clip); return NULL; }
    bind(clip);
    entry->defaults(clip->config);
    clip->effect = entry->make(clip->config);
    TerminalConfig tc;
    terminal_config_default(&tc);
    tc.canvas_width = width;
    tc.canvas_height = height;
    tc.ignore_terminal_dimensions = true;
    tc.anchor_canvas = ANCHOR_NW;
    tc.anchor_text = (Anchor)anchor;
    tc.frame_rate = 60;
    PreprocessError err = {0};
    int status = engine_ctx_init(&clip->ctx, text, &tc, rng_seeded(seed),
                                clock_virtual_with_frame_rate(60), &err);
    // terminal_new cleans its partial allocations on failure.
    clip->initialized = status == 0;
    if (status != 0 || !clip->effect || clip->effect->ops->build(clip->effect, &clip->ctx) != 0) {
        snprintf(error, error_len, "engine initialization failed: %s %s", err.message, err.sequence);
        detach(clip);
        glyphfx_close(clip);
        return NULL;
    }
    detach(clip);
    return clip;
}

const char *glyphfx_next(GlyphfxClip *clip) {
    bind(clip);
    const char *frame = clip->effect->ops->next_frame(clip->effect, &clip->ctx);
    detach(clip);
    return frame;
}
