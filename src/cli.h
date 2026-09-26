// Table-driven CLI parsing. getopt cannot express subcommands, variable
// arity, negative-looking numbers, and per-effect defaults, so the parser is
// hand-rolled around per-command option-spec tables that fill a config struct
// by byte offset.
#ifndef GLYPHFX_CLI_H
#define GLYPHFX_CLI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "engine/terminal.h"

typedef struct {
    bool version;
    const char *input_file;
    bool random_effect;
    const char *effect_name;
    bool has_seed;
    uint64_t seed;
    const char *print_completion;
    bool m0_dump;
    bool parity_dump;
    bool has_max_frames;
    uint64_t max_frames;
    bool virtual_clock;
    TerminalConfig tc;
} CliConfig;

// Returns 0 on success, 2 on usage error (message already written to stderr),
// or 1 for a runtime error that terminates before any input handling.
int cli_parse(int argc, char **argv, CliConfig *cfg);

void cli_print_version(void);

#endif
