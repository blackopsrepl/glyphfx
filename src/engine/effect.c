// Base-effect run loops (reference engine/effect.rs).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/ctx.h"

int effect_dump(Effect *effect, EngineCtx *ctx, bool has_max_frames, uint64_t max_frames) {
    if (effect->ops->build(effect, ctx) != 0) {
        return -1;
    }
    uint64_t count = 0;
    char *frame;
    while ((frame = effect->ops->next_frame(effect, ctx)) != NULL) {
        size_t len = strlen(frame);
        printf("%zu\n", len);
        fwrite(frame, 1, len, stdout);
        fputc('\n', stdout);
        free(frame);
        count++;
        if (has_max_frames && count >= max_frames) {
            break;
        }
    }
    fflush(stdout);
    fprintf(stderr, "frames=%llu\n", (unsigned long long)count);
    return 0;
}

int effect_run(Effect *effect, EngineCtx *ctx, bool tty_output) {
    (void)tty_output;
    if (effect->ops->build(effect, ctx) != 0) {
        return -1;
    }
    terminal_prep_canvas(&ctx->terminal, stdout);
    char *frame;
    while ((frame = effect->ops->next_frame(effect, ctx)) != NULL) {
        terminal_print_frame(&ctx->terminal, stdout, frame);
        free(frame);
    }
    terminal_restore_cursor(&ctx->terminal, stdout, "\n");
    fflush(stdout);
    return 0;
}
