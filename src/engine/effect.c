// Base-effect run loops (reference engine/effect.rs).
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "engine/ctx.h"
#include "utils/signals.h"

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

static bool output_closed(void) {
    // EIO is the pty slave outliving its master; EPIPE means the reader is gone.
    return errno == EIO || errno == EPIPE;
}

static RunOutcome requested_stop(EngineCtx *ctx, bool stop_on_resize) {
    if (interrupted()) {
        return RUN_INTERRUPTED;
    }
    if (terminated()) {
        return RUN_TERMINATED;
    }
    if (stop_on_resize && terminal_resize_settled(&ctx->terminal)) {
        return RUN_RESIZED;
    }
    return RUN_COMPLETE;
}

int effect_run(Effect *effect, EngineCtx *ctx, bool tty_output, RunOutcome *out_outcome) {
    if (effect->ops->build(effect, ctx) != 0) {
        return -1;
    }
    RunOutcome outcome = RUN_COMPLETE;
    int write_error = 0;
    terminal_prep_canvas(&ctx->terminal, stdout);
    for (;;) {
        RunOutcome stop = requested_stop(ctx, tty_output);
        if (stop != RUN_COMPLETE) {
            outcome = stop;
            break;
        }
        char *frame = effect->ops->next_frame(effect, ctx);
        if (!frame) {
            break;
        }
        stop = requested_stop(ctx, tty_output);
        if (stop != RUN_COMPLETE) {
            outcome = stop;
            free(frame);
            break;
        }
        terminal_print_frame(&ctx->terminal, stdout, frame);
        free(frame);
        if (ferror(stdout)) {
            write_error = 1;
            break;
        }
    }
    if (outcome == RUN_RESIZED) {
        terminal_reset_canvas_area(&ctx->terminal, stdout);
    } else {
        terminal_restore_cursor(&ctx->terminal, stdout, "\n");
    }
    if (fflush(stdout) != 0 || ferror(stdout)) {
        write_error = 1;
    }
    if (write_error) {
        if (tty_output && output_closed()) {
            if (out_outcome) {
                *out_outcome = RUN_OUTPUT_CLOSED;
            }
            return 0;
        }
        return -1;
    }
    if (out_outcome) {
        *out_outcome = outcome;
    }
    return 0;
}
