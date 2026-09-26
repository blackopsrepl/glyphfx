#include "cli.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/hexterm.h"

#include "glyphfx_version.h"

typedef enum {
    OPT_FLAG,
    OPT_INT_POS,
    OPT_INT_NONNEG,
    OPT_INT_CANVAS,
    OPT_UINT,
    OPT_COLOR,
    OPT_ANCHOR,
    OPT_EXISTING,
    OPT_STR,
    OPT_COMPLETION,
} OptKind;

typedef struct {
    const char *name;  // long name without the leading "--"
    char short_name;   // 0 if none
    OptKind kind;
    size_t offset;     // offset into CliConfig
    bool hidden;
} OptSpec;

#define OFF(field) offsetof(CliConfig, field)
#define OFF_TC(field) (offsetof(CliConfig, tc) + offsetof(TerminalConfig, field))

static const OptSpec ROOT_OPTS[] = {
    {"version", 'v', OPT_FLAG, OFF(version), false},
    {"input-file", 'i', OPT_STR, OFF(input_file), false},
    {"tab-width", 0, OPT_INT_POS, OFF_TC(tab_width), false},
    {"xterm-colors", 0, OPT_FLAG, OFF_TC(xterm_colors), false},
    {"no-color", 0, OPT_FLAG, OFF_TC(no_color), false},
    {"terminal-background-color", 0, OPT_COLOR, OFF_TC(terminal_background_color), false},
    {"existing-color-handling", 0, OPT_EXISTING, OFF_TC(existing_color_handling), false},
    {"wrap-text", 0, OPT_FLAG, OFF_TC(wrap_text), false},
    {"frame-rate", 0, OPT_INT_NONNEG, OFF_TC(frame_rate), false},
    {"canvas-width", 0, OPT_INT_CANVAS, OFF_TC(canvas_width), false},
    {"canvas-height", 0, OPT_INT_CANVAS, OFF_TC(canvas_height), false},
    {"anchor-canvas", 0, OPT_ANCHOR, OFF_TC(anchor_canvas), false},
    {"anchor-text", 0, OPT_ANCHOR, OFF_TC(anchor_text), false},
    {"ignore-terminal-dimensions", 0, OPT_FLAG, OFF_TC(ignore_terminal_dimensions), false},
    {"reuse-canvas", 0, OPT_FLAG, OFF_TC(reuse_canvas), false},
    {"no-eol", 0, OPT_FLAG, OFF_TC(no_eol), false},
    {"no-restore-cursor", 0, OPT_FLAG, OFF_TC(no_restore_cursor), false},
    {"seed", 0, OPT_UINT, OFF(seed), false},
    {"print-completion", 0, OPT_COMPLETION, OFF(print_completion), false},
    {"random-effect", 'R', OPT_FLAG, OFF(random_effect), false},
    {"m0-dump", 0, OPT_FLAG, OFF(m0_dump), true},
    {"parity-dump", 0, OPT_FLAG, OFF(parity_dump), true},
    {"max-frames", 0, OPT_UINT, OFF(max_frames), true},
    {"virtual-clock", 0, OPT_FLAG, OFF(virtual_clock), true},
};

static const size_t ROOT_OPTS_COUNT = sizeof(ROOT_OPTS) / sizeof(ROOT_OPTS[0]);

static void usage_error(const char *fmt, ...) {
    fprintf(stderr, "error: ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
    fprintf(stderr, "Try 'glyphfx --help' for more information.\n");
}

static const OptSpec *find_long(const char *name) {
    for (size_t i = 0; i < ROOT_OPTS_COUNT; i++) {
        if (strcmp(ROOT_OPTS[i].name, name) == 0) {
            return &ROOT_OPTS[i];
        }
    }
    return NULL;
}

static const OptSpec *find_short(char c) {
    for (size_t i = 0; i < ROOT_OPTS_COUNT; i++) {
        if (ROOT_OPTS[i].short_name == c) {
            return &ROOT_OPTS[i];
        }
    }
    return NULL;
}

static int parse_i64(const char *s, int64_t *out) {
    if (s[0] == '\0') {
        return -1;
    }
    char *endp = NULL;
    errno = 0;
    long long v = strtoll(s, &endp, 10);
    if (errno != 0 || !endp || *endp != '\0') {
        return -1;
    }
    *out = (int64_t)v;
    return 0;
}

static int parse_u64(const char *s, uint64_t *out) {
    if (s[0] == '\0' || s[0] == '-') {
        return -1;
    }
    char *endp = NULL;
    errno = 0;
    unsigned long long v = strtoull(s, &endp, 10);
    if (errno != 0 || !endp || *endp != '\0') {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

static int parse_color_arg(const char *s, Color *out) {
    size_t len = strlen(s);
    if (len <= 3) {
        int64_t code = 0;
        if (parse_i64(s, &code) != 0 || code < 0 || code > 255) {
            return -1;
        }
        *out = color_from_xterm((uint8_t)code);
        return 0;
    }
    return color_from_hex(s, out);
}

static int parse_existing(const char *s, ExistingColorHandling *out) {
    if (strcmp(s, "always") == 0) {
        *out = EXISTING_COLOR_ALWAYS;
    } else if (strcmp(s, "dynamic") == 0) {
        *out = EXISTING_COLOR_DYNAMIC;
    } else if (strcmp(s, "ignore") == 0) {
        *out = EXISTING_COLOR_IGNORE;
    } else {
        return -1;
    }
    return 0;
}

static void set_flag(CliConfig *cfg, const OptSpec *spec) {
    *(bool *)((char *)cfg + spec->offset) = true;
}

static int assign_option(CliConfig *cfg, const OptSpec *spec, const char *value) {
    char *base = (char *)cfg;
    switch (spec->kind) {
        case OPT_FLAG:
            set_flag(cfg, spec);
            return 0;
        case OPT_STR:
            *(const char **)(base + spec->offset) = value;
            return 0;
        case OPT_COMPLETION:
            *(const char **)(base + spec->offset) = value;
            return 0;
        case OPT_INT_POS: {
            int64_t v;
            if (parse_i64(value, &v) != 0 || v <= 0) {
                usage_error("invalid value '%s' for a positive integer option", value);
                return 2;
            }
            *(int64_t *)(base + spec->offset) = v;
            return 0;
        }
        case OPT_INT_NONNEG: {
            int64_t v;
            if (parse_i64(value, &v) != 0 || v < 0) {
                usage_error("invalid value '%s' for a non-negative integer option", value);
                return 2;
            }
            *(int64_t *)(base + spec->offset) = v;
            return 0;
        }
        case OPT_INT_CANVAS: {
            int64_t v;
            if (parse_i64(value, &v) != 0 || v < -1) {
                usage_error("invalid value '%s' for a canvas dimension", value);
                return 2;
            }
            *(int64_t *)(base + spec->offset) = v;
            return 0;
        }
        case OPT_UINT: {
            uint64_t v;
            if (parse_u64(value, &v) != 0) {
                usage_error("invalid value '%s' for an unsigned integer option", value);
                return 2;
            }
            *(uint64_t *)(base + spec->offset) = v;
            if (spec->offset == OFF(seed)) {
                cfg->has_seed = true;
            } else if (spec->offset == OFF(max_frames)) {
                cfg->has_max_frames = true;
            }
            return 0;
        }
        case OPT_COLOR: {
            Color c;
            if (parse_color_arg(value, &c) != 0) {
                usage_error("invalid color value '%s'", value);
                return 2;
            }
            *(Color *)(base + spec->offset) = c;
            return 0;
        }
        case OPT_ANCHOR: {
            Anchor a;
            if (!anchor_parse(value, &a)) {
                usage_error("invalid anchor '%s'", value);
                return 2;
            }
            *(Anchor *)(base + spec->offset) = a;
            return 0;
        }
        case OPT_EXISTING: {
            ExistingColorHandling h;
            if (parse_existing(value, &h) != 0) {
                usage_error("invalid choice '%s' (choose from 'always', 'dynamic', 'ignore')", value);
                return 2;
            }
            *(ExistingColorHandling *)(base + spec->offset) = h;
            return 0;
        }
    }
    return 2;
}

int cli_parse(int argc, char **argv, CliConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    terminal_config_default(&cfg->tc);

    bool positional_seen = false;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (positional_seen) {
            // Effect options are parsed by the effect's own table (M3).
            continue;
        }
        if (strcmp(arg, "--") == 0) {
            if (i + 1 < argc) {
                cfg->effect_name = argv[i + 1];
                positional_seen = true;
            }
            continue;
        }
        if (arg[0] == '-' && arg[1] == '-' && arg[2] != '\0') {
            const char *name = arg + 2;
            const char *eq = strchr(name, '=');
            char namebuf[64];
            const char *value = NULL;
            if (eq) {
                size_t n = (size_t)(eq - name);
                if (n >= sizeof(namebuf)) {
                    usage_error("unrecognized option '%s'", arg);
                    return 2;
                }
                memcpy(namebuf, name, n);
                namebuf[n] = '\0';
                name = namebuf;
                value = eq + 1;
            }
            const OptSpec *spec = find_long(name);
            if (!spec) {
                usage_error("unrecognized option '%s'", arg);
                return 2;
            }
            if (spec->kind == OPT_FLAG) {
                if (value) {
                    usage_error("option '--%s' does not take a value", spec->name);
                    return 2;
                }
                set_flag(cfg, spec);
            } else {
                if (!value) {
                    if (i + 1 >= argc) {
                        usage_error("option '--%s' requires a value", spec->name);
                        return 2;
                    }
                    value = argv[++i];
                }
                int rc = assign_option(cfg, spec, value);
                if (rc != 0) {
                    return rc;
                }
            }
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0' && arg[1] != '-') {
            // Single short option; only -v, -i, -R exist.
            if (arg[2] != '\0') {
                usage_error("unrecognized option '%s'", arg);
                return 2;
            }
            const OptSpec *spec = find_short(arg[1]);
            if (!spec) {
                usage_error("unrecognized option '%s'", arg);
                return 2;
            }
            if (spec->kind == OPT_FLAG) {
                set_flag(cfg, spec);
            } else {
                if (i + 1 >= argc) {
                    usage_error("option '-%c' requires a value", arg[1]);
                    return 2;
                }
                int rc = assign_option(cfg, spec, argv[++i]);
                if (rc != 0) {
                    return rc;
                }
            }
            continue;
        }
        // First positional argument names the effect.
        cfg->effect_name = arg;
        positional_seen = true;
    }
    return 0;
}

void cli_print_version(void) {
    printf("glyphfx %s\n", GLYPHFX_VERSION);
}
