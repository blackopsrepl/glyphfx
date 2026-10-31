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

// List types shared by root and effect option tables.
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

// A list of single-codepoint symbols (reference Vec<String> parse_symbol).
// Items are interned (strtab) and owned by the table: the list frees only
// its array, and an item passed to a name API resolves without hashing.
typedef struct {
    char **items;
    size_t len;
    size_t cap;
    bool provided;
} StringList;

typedef struct {
    bool version;
    bool help;
    bool help_effect;
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
    StringList include_effects;
    StringList exclude_effects;
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
    EF_SYMBOL,
    EF_COLOR,
    EF_DIRECTION,
    EF_EASING,
    EF_CHAR_SORT,
    EF_CHAR_GROUP,
    EF_COLOR_LIST,
    EF_INT_LIST,
    EF_STRING_LIST,
    EF_INT_RANGE,
    EF_FLOAT_RANGE,
    EF_CUSTOM,
} EffKind;

typedef struct {
    int64_t start;
    int64_t end;
} IntRange;

typedef struct {
    double start;
    double end;
} FloatRange;

typedef struct {
    const char *name;  // long name without the leading "--"
    char short_name;   // 0 if none
    EffKind kind;
    size_t offset;     // offset into the effect config struct
    // For EF_CUSTOM: fills base+offset. Returns 0 on success.
    int (*custom)(const char *value, void *dst);
} EffOptSpec;

// Designated initializer so the trailing custom field needs no mention.
#define EF_SPEC(n, sc, k, o) \
    { .name = (n), .short_name = (sc), .kind = (k), .offset = (o), .custom = NULL }

// How help text is drawn: colour (a terminal that wants it) and the wrap
// width (0 keeps every entry on one line, for pipes and grep).
typedef struct {
    bool color;
    int width;
} CliStyle;

// Returns 0 on success, 2 on usage error (message already written to stderr),
// or 1 for a runtime error that terminates before any input handling.
int cli_parse(int argc, char **argv, CliConfig *cfg);

void cli_print_version(void);
// after_logo: the wordmark was just drawn, so the header is only the version
// line beneath it.
void cli_print_help(const CliStyle *style, bool after_logo);
void cli_print_effect_help(const void *effect_entry, const CliStyle *style);
void cli_print_completion(const char *shell);
const char *cli_effect_name(const void *effect_entry);
// Writes every option the parser accepts but the help does not describe (and
// every described option the parser does not accept) to stderr; true when
// there are none.
bool cli_help_audit(void);

#endif
