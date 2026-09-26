#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cli.h"
#include "effects/registry.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
#include "utils/clock.h"
#include "utils/rng.h"
#include "utils/signals.h"
#include "utils/utf8.h"

static char *read_all(FILE *f, size_t *out_len) {
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }
    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            char *grown = realloc(buf, cap);
            if (!grown) {
                free(buf);
                return NULL;
            }
            buf = grown;
        }
        size_t n = fread(buf + len, 1, cap - len, f);
        len += n;
        if (n == 0) {
            break;
        }
    }
    buf[len] = '\0';
    *out_len = len;
    return buf;
}

// Rust str::trim is Unicode-aware; whitespace-only input is rejected. This
// covers the Unicode White_Space codepoints that appear in practice.
static bool is_unicode_whitespace(uint32_t cp) {
    switch (cp) {
        case 0x09:
        case 0x0A:
        case 0x0B:
        case 0x0C:
        case 0x0D:
        case 0x20:
        case 0x85:
        case 0xA0:
        case 0x1680:
        case 0x2028:
        case 0x2029:
        case 0x202F:
        case 0x205F:
        case 0x3000:
            return true;
        default:
            return cp >= 0x2000 && cp <= 0x200A;
    }
}

static bool is_whitespace_only(const uint32_t *cps, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (!is_unicode_whitespace(cps[i])) {
            return false;
        }
    }
    return true;
}

static int run_m0_dump(const char *input, const CliConfig *cfg) {
    Terminal terminal;
    PreprocessError err;
    memset(&err, 0, sizeof(err));
    if (terminal_new(&terminal, input, &cfg->tc, &err) != 0) {
        if (err.status == PREPROCESS_UNSUPPORTED_ANSI) {
            fprintf(stderr, "Error: Unsupported ANSI sequence in input data: \"%s\"\n", err.sequence);
        } else {
            fprintf(stderr, "Error: %s\n", err.message[0] ? err.message : "failed to build canvas");
        }
        return 1;
    }
    size_t cells = (size_t)(terminal.map_right + 1) * (size_t)(terminal.map_top + 1);
    for (size_t i = 0; i < cells; i++) {
        CharId id = terminal.coord_map[i];
        if (id != CHAR_ID_NONE) {
            terminal_set_character_visibility(&terminal, id, true);
        }
    }
    char *frame = terminal_get_formatted_output_string(&terminal);
    fputs(frame, stdout);
    fputc('\n', stdout);
    free(frame);
    terminal_free(&terminal);
    return 0;
}

int main(int argc, char **argv) {
    restore_sigpipe();

    CliConfig cfg;
    int rc = cli_parse(argc, argv, &cfg);
    if (rc != 0) {
        return rc;
    }
    if (cfg.version) {
        cli_print_version();
        return 0;
    }
    if (cfg.help) {
        cli_print_help();
        return 0;
    }
    if (cfg.help_effect) {
        cli_print_effect_help(cfg.effect_entry);
        return 0;
    }
    if (cfg.print_completion) {
        cli_print_completion(cfg.print_completion);
        return 0;
    }

    char *input = NULL;
    size_t input_len = 0;
    if (cfg.input_file) {
        FILE *f = fopen(cfg.input_file, "rb");
        if (!f) {
            printf("Error reading input file: %s\n", strerror(errno));
            return 1;
        }
        input = read_all(f, &input_len);
        fclose(f);
        if (!input) {
            printf("Error reading input file: out of memory\n");
            return 1;
        }
    } else if (isatty(STDIN_FILENO)) {
        input = calloc(1, 1);
        input_len = 0;
    } else {
        input = read_all(stdin, &input_len);
        if (!input) {
            printf("Error decoding input: out of memory\n");
            return 1;
        }
    }

    uint32_t *cps = NULL;
    size_t cp_count = 0;
    if (utf8_decode_strict(input, input_len, &cps, &cp_count, NULL) != 0) {
        printf("Error decoding input: invalid utf-8 sequence\n");
        free(input);
        return 1;
    }

    if (is_whitespace_only(cps, cp_count)) {
        printf("NO INPUT.\n");
        free(cps);
        free(input);
        return 1;
    }
    free(cps);

    if (cfg.m0_dump) {
        int m0 = run_m0_dump(input, &cfg);
        if (cfg.effect_entry) {
            ((const EffectEntry *)cfg.effect_entry)->free_config(cfg.effect_config);
            free(cfg.effect_config);
        }
        free(input);
        return m0;
    }

    if (!cfg.effect_entry && !cfg.random_effect) {
        fputs("Error: No effect specified.\n", stderr);
        free(input);
        return 1;
    }

    Rng rng = cfg.has_seed ? rng_seeded(cfg.seed) : rng_from_entropy();

    if (cfg.random_effect) {
        const EffectEntry *names[128];
        size_t name_count = 0;
        for (size_t i = 0; i < effect_entry_count(); i++) {
            const EffectEntry *entry = effect_entry_at(i);
            if (!entry) {
                continue;
            }
            if (cfg.include_effects.provided) {
                bool included = false;
                for (size_t k = 0; k < cfg.include_effects.len; k++) {
                    if (strcmp(cfg.include_effects.items[k], entry->name) == 0) {
                        included = true;
                        break;
                    }
                }
                if (!included) {
                    continue;
                }
            }
            bool excluded = false;
            for (size_t k = 0; k < cfg.exclude_effects.len; k++) {
                if (strcmp(cfg.exclude_effects.items[k], entry->name) == 0) {
                    excluded = true;
                    break;
                }
            }
            if (excluded) {
                continue;
            }
            if (name_count < 128) {
                names[name_count++] = entry;
            }
        }
        if (name_count == 0) {
            fputs("Error: No effects available after filtering.\n", stderr);
            free(input);
            return 1;
        }
        const EffectEntry *picked = names[rng_choice_index(&rng, name_count)];
        cfg.effect_entry = picked;
        if (cfg.effect_config) {
            picked->free_config(cfg.effect_config);
            free(cfg.effect_config);
        }
        cfg.effect_config = calloc(1, picked->config_size);
        if (!cfg.effect_config) {
            free(input);
            return 1;
        }
        picked->defaults(cfg.effect_config);
    }

    const EffectEntry *entry = (const EffectEntry *)cfg.effect_entry;

    bool tty_output = !cfg.parity_dump && isatty(STDOUT_FILENO) != 0;
    if (!cfg.parity_dump) {
        install_sigint_handler();
    }
    if (tty_output) {
        install_sigterm_handler();
        install_sigwinch_handler();
    }

    TerminalConfig config = cfg.tc;
    int exit_code = 0;
    for (;;) {
        Effect *effect = entry->make(cfg.effect_config);
        if (!effect) {
            fputs("Error: failed to build effect.\n", stderr);
            exit_code = 1;
            break;
        }
        Clock clock = (cfg.parity_dump || cfg.virtual_clock) ? clock_virtual_with_frame_rate(config.frame_rate)
                                                             : clock_real();
        EngineCtx ctx;
        PreprocessError pp_err;
        memset(&pp_err, 0, sizeof(pp_err));
        if (engine_ctx_init(&ctx, input, &config, rng, clock, &pp_err) != 0) {
            if (pp_err.status == PREPROCESS_UNSUPPORTED_ANSI) {
                fprintf(stderr, "Error: Unsupported ANSI sequence in input data: \"%s\"\n", pp_err.sequence);
            } else {
                fprintf(stderr, "Error: %s\n", pp_err.message[0] ? pp_err.message : "failed to build canvas");
            }
            effect->ops->destroy(effect);
            exit_code = 1;
            break;
        }
        if (cfg.parity_dump) {
            int run_rc = effect_dump(effect, &ctx, cfg.has_max_frames, cfg.max_frames);
            effect->ops->destroy(effect);
            engine_ctx_free(&ctx);
            if (run_rc != 0) {
                fputs("Error: effect execution failed.\n", stderr);
                exit_code = 1;
            }
            break;
        }
        RunOutcome outcome = RUN_COMPLETE;
        int run_rc = effect_run(effect, &ctx, tty_output, &outcome);
        rng = ctx.rng;
        effect->ops->destroy(effect);
        engine_ctx_free(&ctx);
        if (run_rc != 0) {
            fputs("Error: effect execution failed.\n", stderr);
            exit_code = 1;
            break;
        }
        if (outcome == RUN_RESIZED) {
            config.reuse_canvas = false;
            continue;
        }
        if (outcome == RUN_INTERRUPTED) {
            exit_code = 1;
        }
        break;
    }

    entry->free_config(cfg.effect_config);
    free(cfg.effect_config);
    free(input);
    if (terminated()) {
        die_from_sigterm();
    }
    if (interrupted() && exit_code == 0) {
        exit_code = 1;
    }
    return exit_code;
}
