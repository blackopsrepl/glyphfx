#include "cli.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "effects/registry.h"
#include "utils/hexterm.h"
#include "utils/utf8.h"

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

static void list_color_append(ColorList *list, Color c) {
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 4;
        Color *grown = realloc(list->items, cap * sizeof(Color));
        if (!grown) {
            return;
        }
        list->items = grown;
        list->cap = cap;
    }
    list->items[list->len++] = c;
}

static void list_int_append(IntList *list, int64_t v) {
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 4;
        int64_t *grown = realloc(list->items, cap * sizeof(int64_t));
        if (!grown) {
            return;
        }
        list->items = grown;
        list->cap = cap;
    }
    list->items[list->len++] = v;
}

static int parse_double_arg(const char *s, double *out) {
    if (s[0] == '\0') {
        return -1;
    }
    char *endp = NULL;
    errno = 0;
    double v = strtod(s, &endp);
    if (errno != 0 || !endp || *endp != '\0') {
        return -1;
    }
    *out = v;
    return 0;
}

static const EffOptSpec *eff_find(const EffectEntry *entry, const char *name) {
    for (size_t i = 0; i < entry->n_specs; i++) {
        if (strcmp(entry->specs[i].name, name) == 0) {
            return &entry->specs[i];
        }
    }
    return NULL;
}

static int eff_assign(const EffectEntry *entry, void *cfg, const EffOptSpec *spec, const char *value) {
    char *base = cfg;
    (void)entry;
    switch (spec->kind) {
        case EF_FLAG:
            *(bool *)(base + spec->offset) = true;
            return 0;
        case EF_INT: {
            int64_t v;
            if (parse_i64(value, &v) != 0) {
                usage_error("invalid int value '%s'", value);
                return 2;
            }
            *(int64_t *)(base + spec->offset) = v;
            return 0;
        }
        case EF_POS_INT: {
            int64_t v;
            if (parse_i64(value, &v) != 0 || v <= 0) {
                usage_error("invalid value '%s': must be an int > 0", value);
                return 2;
            }
            *(int64_t *)(base + spec->offset) = v;
            return 0;
        }
        case EF_NONNEG_INT: {
            int64_t v;
            if (parse_i64(value, &v) != 0 || v < 0) {
                usage_error("invalid value '%s': must be an int >= 0", value);
                return 2;
            }
            *(int64_t *)(base + spec->offset) = v;
            return 0;
        }
        case EF_FLOAT_POS: {
            double v;
            if (parse_double_arg(value, &v) != 0 || !(v > 0.0)) {
                usage_error("invalid value '%s': must be a float > 0", value);
                return 2;
            }
            *(double *)(base + spec->offset) = v;
            return 0;
        }
        case EF_FLOAT_NONNEG: {
            double v;
            if (parse_double_arg(value, &v) != 0 || !(v >= 0.0)) {
                usage_error("invalid value '%s': must be a float >= 0", value);
                return 2;
            }
            *(double *)(base + spec->offset) = v;
            return 0;
        }
        case EF_RATIO_NONNEG: {
            double v;
            if (parse_double_arg(value, &v) != 0 || !(v >= 0.0 && v <= 1.0)) {
                usage_error("invalid value '%s': must be a float 0 <= n <= 1", value);
                return 2;
            }
            *(double *)(base + spec->offset) = v;
            return 0;
        }
        case EF_RATIO_POS: {
            double v;
            if (parse_double_arg(value, &v) != 0 || !(v > 0.0 && v <= 1.0)) {
                usage_error("invalid value '%s': must be a float 0 < n <= 1", value);
                return 2;
            }
            *(double *)(base + spec->offset) = v;
            return 0;
        }
        case EF_STRING:
            *(const char **)(base + spec->offset) = value;
            return 0;
        case EF_SYMBOL: {
            if (utf8_count_codepoints(value) != 1) {
                usage_error("invalid symbol '%s': must be a single character", value);
                return 2;
            }
            char **dst = (char **)(base + spec->offset);
            free(*dst);
            *dst = malloc(strlen(value) + 1);
            strcpy(*dst, value);
            return 0;
        }
        case EF_COLOR: {
            Color c;
            if (parse_color_arg(value, &c) != 0) {
                usage_error("invalid color value '%s'", value);
                return 2;
            }
            *(Color *)(base + spec->offset) = c;
            return 0;
        }
        case EF_DIRECTION: {
            GradientDirection d;
            if (strcmp(value, "horizontal") == 0) {
                d = GRADIENT_HORIZONTAL;
            } else if (strcmp(value, "vertical") == 0) {
                d = GRADIENT_VERTICAL;
            } else if (strcmp(value, "diagonal") == 0) {
                d = GRADIENT_DIAGONAL;
            } else if (strcmp(value, "radial") == 0) {
                d = GRADIENT_RADIAL;
            } else {
                usage_error("invalid gradient direction '%s'", value);
                return 2;
            }
            *(GradientDirection *)(base + spec->offset) = d;
            return 0;
        }
        case EF_COLOR_LIST: {
            ColorList *list = (ColorList *)(base + spec->offset);
            if (!list->provided) {
                list->len = 0;
                list->provided = true;
            }
            Color c;
            if (parse_color_arg(value, &c) != 0) {
                usage_error("invalid color value '%s'", value);
                return 2;
            }
            list_color_append(list, c);
            return 0;
        }
        case EF_INT_LIST: {
            IntList *list = (IntList *)(base + spec->offset);
            if (!list->provided) {
                list->len = 0;
                list->provided = true;
            }
            int64_t v;
            if (parse_i64(value, &v) != 0 || v <= 0) {
                usage_error("invalid value '%s': must be an int > 0", value);
                return 2;
            }
            list_int_append(list, v);
            return 0;
        }
        case EF_STRING_LIST: {
            StringList *list = (StringList *)(base + spec->offset);
            if (utf8_count_codepoints(value) != 1) {
                usage_error("invalid symbol '%s': must be a single character", value);
                return 2;
            }
            if (!list->provided) {
                for (size_t i = 0; i < list->len; i++) {
                    free(list->items[i]);
                }
                list->len = 0;
                list->provided = true;
            }
            if (list->len == list->cap) {
                size_t cap = list->cap ? list->cap * 2 : 4;
                list->items = realloc(list->items, cap * sizeof(char *));
                list->cap = cap;
            }
            list->items[list->len] = malloc(strlen(value) + 1);
            strcpy(list->items[list->len], value);
            list->len++;
            return 0;
        }
        case EF_EASING: {
            Easing e;
            if (!easing_parse(value, &e)) {
                usage_error("invalid easing function '%s'", value);
                return 2;
            }
            *(Easing *)(base + spec->offset) = e;
            return 0;
        }
        case EF_CHAR_SORT: {
            CharacterSort s;
            if (strcmp(value, "random") == 0) s = CS_RANDOM;
            else if (strcmp(value, "top_to_bottom_left_to_right") == 0) s = CS_TOP_TO_BOTTOM_LEFT_TO_RIGHT;
            else if (strcmp(value, "top_to_bottom_right_to_left") == 0) s = CS_TOP_TO_BOTTOM_RIGHT_TO_LEFT;
            else if (strcmp(value, "bottom_to_top_left_to_right") == 0) s = CS_BOTTOM_TO_TOP_LEFT_TO_RIGHT;
            else if (strcmp(value, "bottom_to_top_right_to_left") == 0) s = CS_BOTTOM_TO_TOP_RIGHT_TO_LEFT;
            else if (strcmp(value, "outside_row_to_middle") == 0) s = CS_OUTSIDE_ROW_TO_MIDDLE;
            else if (strcmp(value, "middle_row_to_outside") == 0) s = CS_MIDDLE_ROW_TO_OUTSIDE;
            else {
                usage_error("invalid character sort '%s'", value);
                return 2;
            }
            *(CharacterSort *)(base + spec->offset) = s;
            return 0;
        }
        case EF_CHAR_GROUP: {
            CharacterGroup grp;
            if (strcmp(value, "column_left_to_right") == 0) grp = CG_COLUMN_LEFT_TO_RIGHT;
            else if (strcmp(value, "column_right_to_left") == 0) grp = CG_COLUMN_RIGHT_TO_LEFT;
            else if (strcmp(value, "row_top_to_bottom") == 0) grp = CG_ROW_TOP_TO_BOTTOM;
            else if (strcmp(value, "row_bottom_to_top") == 0) grp = CG_ROW_BOTTOM_TO_TOP;
            else if (strcmp(value, "diagonal_top_left_to_bottom_right") == 0) grp = CG_DIAGONAL_TOP_LEFT_TO_BOTTOM_RIGHT;
            else if (strcmp(value, "diagonal_bottom_left_to_top_right") == 0) grp = CG_DIAGONAL_BOTTOM_LEFT_TO_TOP_RIGHT;
            else if (strcmp(value, "diagonal_top_right_to_bottom_left") == 0) grp = CG_DIAGONAL_TOP_RIGHT_TO_BOTTOM_LEFT;
            else if (strcmp(value, "diagonal_bottom_right_to_top_left") == 0) grp = CG_DIAGONAL_BOTTOM_RIGHT_TO_TOP_LEFT;
            else if (strcmp(value, "center_to_outside") == 0) grp = CG_CENTER_TO_OUTSIDE;
            else if (strcmp(value, "outside_to_center") == 0) grp = CG_OUTSIDE_TO_CENTER;
            else {
                usage_error("invalid character group '%s'", value);
                return 2;
            }
            *(CharacterGroup *)(base + spec->offset) = grp;
            return 0;
        }
        case EF_INT_RANGE: {
            const char *dash = strchr(value, '-');
            if (!dash || dash == value) {
                usage_error("invalid range '%s'", value);
                return 2;
            }
            char a[32];
            size_t alen = (size_t)(dash - value);
            if (alen >= sizeof(a)) {
                usage_error("invalid range '%s'", value);
                return 2;
            }
            memcpy(a, value, alen);
            a[alen] = '\0';
            int64_t lo, hi;
            if (parse_i64(a, &lo) != 0 || parse_i64(dash + 1, &hi) != 0 || lo <= 0 || lo > hi) {
                usage_error("invalid range '%s'", value);
                return 2;
            }
            IntRange *r = (IntRange *)(base + spec->offset);
            r->start = lo;
            r->end = hi;
            return 0;
        }
        case EF_FLOAT_RANGE: {
            const char *dash = strchr(value, '-');
            if (!dash || dash == value) {
                usage_error("invalid range '%s'", value);
                return 2;
            }
            char a[64];
            size_t alen = (size_t)(dash - value);
            if (alen >= sizeof(a)) {
                usage_error("invalid range '%s'", value);
                return 2;
            }
            memcpy(a, value, alen);
            a[alen] = '\0';
            double lo, hi;
            if (parse_double_arg(a, &lo) != 0 || parse_double_arg(dash + 1, &hi) != 0 || !(lo > 0.0) ||
                !(lo <= hi)) {
                usage_error("invalid range '%s'", value);
                return 2;
            }
            FloatRange *r = (FloatRange *)(base + spec->offset);
            r->start = lo;
            r->end = hi;
            return 0;
        }
        case EF_CUSTOM: {
            if (!spec->custom || spec->custom(value, base + spec->offset) != 0) {
                usage_error("invalid value '%s' for option '%s'", value, spec->name);
                return 2;
            }
            return 0;
        }
    }
    return 2;
}

static bool looks_like_option(const char *s) {
    return s[0] == '-' && s[1] != '\0';
}

static int parse_effect_args(const EffectEntry *entry, void *cfg, int argc, char **argv, int start) {
    for (int i = start; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--") == 0) {
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
            const EffOptSpec *spec = eff_find(entry, name);
            if (!spec) {
                usage_error("unrecognized option '--%s' for effect '%s'", name, entry->name);
                return 2;
            }
            if (spec->kind == EF_FLAG) {
                int rc = eff_assign(entry, cfg, spec, "1");
                if (rc != 0) {
                    return rc;
                }
                continue;
            }
            if (spec->kind == EF_COLOR_LIST || spec->kind == EF_INT_LIST || spec->kind == EF_STRING_LIST) {
                // Consume one or more values until the next option.
                int consumed = 0;
                if (value) {
                    int rc = eff_assign(entry, cfg, spec, value);
                    if (rc != 0) {
                        return rc;
                    }
                    consumed++;
                }
                while (i + 1 < argc && !looks_like_option(argv[i + 1])) {
                    int rc = eff_assign(entry, cfg, spec, argv[++i]);
                    if (rc != 0) {
                        return rc;
                    }
                    consumed++;
                }
                if (consumed == 0) {
                    usage_error("option '--%s' requires at least one value", name);
                    return 2;
                }
                continue;
            }
            if (!value) {
                if (i + 1 >= argc) {
                    usage_error("option '--%s' requires a value", name);
                    return 2;
                }
                value = argv[++i];
            }
            int rc = eff_assign(entry, cfg, spec, value);
            if (rc != 0) {
                return rc;
            }
            continue;
        }
        usage_error("unrecognized argument '%s' for effect '%s'", arg, entry->name);
        return 2;
    }
    return 0;
}

int cli_parse(int argc, char **argv, CliConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    terminal_config_default(&cfg->tc);

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--") == 0) {
            if (i + 1 >= argc) {
                break;
            }
            arg = argv[++i];
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
        // First positional argument names the effect; the rest are its options.
        cfg->effect_name = arg;
        const EffectEntry *entry = effect_find(arg);
        if (!entry) {
            usage_error("unrecognized effect '%s'", arg);
            return 2;
        }
        cfg->effect_entry = entry;
        cfg->effect_config = calloc(1, entry->config_size);
        if (!cfg->effect_config) {
            return 1;
        }
        entry->defaults(cfg->effect_config);
        int effect_rc = parse_effect_args(entry, cfg->effect_config, argc, argv, i + 1);
        if (effect_rc != 0) {
            return effect_rc;
        }
        break;
    }
    return 0;
}

void cli_print_version(void) {
    printf("glyphfx %s\n", GLYPHFX_VERSION);
}
