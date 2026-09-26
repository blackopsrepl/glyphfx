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
    // Resolved effect: its registry entry and parsed config (owned).
    const void *effect_entry;
    void *effect_config;
} CliConfig;

// --- effect option surface ------------------------------------------------

typedef enum {
    EF_FLAG,
    EF_INT,
    EF_POS_INT,
    EF_NONNEG_INT,
    EF_FLOAT_POS,
    EF_FLOAT_NONNEG,
    EF_RATIO_NONNEG,
    EF_RATIO_POS,
    EF_STRING,
    EF_COLOR,
    EF_DIRECTION,
    EF_EASING,
    EF_CHAR_SORT,
    EF_CHAR_GROUP,
    EF_COLOR_LIST,
    EF_INT_LIST,
} EffKind;

typedef struct {
    const char *name;  // long name without the leading "--"
    char short_name;   // 0 if none
    EffKind kind;
    size_t offset;     // offset into the effect config struct
} EffOptSpec;

typedef struct {
    Color *items;
    size_t len;
    size_t cap;
    bool provided;
} ColorList;

typedef struct {
    int64_t *items;
    size_t len;
    size_t cap;
    bool provided;
} IntList;

// Returns 0 on success, 2 on usage error (message already written to stderr),
// or 1 for a runtime error that terminates before any input handling.
int cli_parse(int argc, char **argv, CliConfig *cfg);

void cli_print_version(void);

#endif
