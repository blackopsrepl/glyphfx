// Help text for every option and effect, printed by `glyphfx --help` and
// `glyphfx <effect> --help`. The tables live in cli_help.c, which
// tools/gen_cli_help.py generates from the reference ttfx --help and
// docs/ORIGINALS.md; tests/test_cli_help.c keeps them in step with the
// option tables the parser reads.
#ifndef GLYPHFX_CLI_HELP_H
#define GLYPHFX_CLI_HELP_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *name;   // long name without the leading "--"
    char short_name;    // 0 if none
    const char *value;  // value placeholder, or NULL for a flag
    bool multi;         // takes one or more values
    const char *help;   // one sentence, may be empty
    const char *def;    // default value, or NULL
} CliOptHelp;

typedef struct {
    const char *effect;
    const char *about;
    const CliOptHelp *opts;
    size_t n_opts;
} CliEffectHelp;

extern const CliOptHelp CLI_ROOT_HELP[];
extern const size_t CLI_ROOT_HELP_COUNT;
extern const CliEffectHelp CLI_EFFECT_HELP[];
extern const size_t CLI_EFFECT_HELP_COUNT;

const CliEffectHelp *cli_help_effect(const char *name);
const CliOptHelp *cli_help_option(const CliOptHelp *opts, size_t n, const char *name);

#endif
