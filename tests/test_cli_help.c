// Help text: every option the parser accepts is described, every described
// option exists, and every registered effect has its one-line description.
// tools/gen_cli_help.py writes the table; this keeps it honest when an option
// is added, renamed or removed.
#include <stdio.h>
#include <string.h>

#include "cli.h"
#include "cli_help.h"
#include "effects/registry.h"
#include "testutil.h"

// The registry declares entries weak, so a weak reference alone does not
// pull an effect out of libglyphfx.a. A strong reference to each
// constructor links all 41 (volatile, so the references are not folded away).
#define DECLARE_MAKE(name) Effect *name##_make(const void *cfg);
GLYPHFX_EFFECT_NAMES(DECLARE_MAKE)
GLYPHFX_ORIGINAL_EFFECT_NAMES(DECLARE_MAKE)
#define REF_MAKE(name) name##_make,
static Effect *(*volatile LINKED[])(const void *) = {
    GLYPHFX_EFFECT_NAMES(REF_MAKE) GLYPHFX_ORIGINAL_EFFECT_NAMES(REF_MAKE)};

int main(void) {
    CHECK_EQ_INT(sizeof(LINKED) / sizeof(LINKED[0]), 41);
    for (size_t i = 0; i < effect_entry_count(); i++) {
        CHECK(effect_entry_at(i) != NULL);
    }
    CHECK(cli_help_audit());
    CHECK_EQ_INT(CLI_EFFECT_HELP_COUNT, effect_entry_count());

    // Every description is a sentence of its own, and a default never carries
    // its [default: ...] wrapper (the renderer adds it).
    for (size_t i = 0; i < CLI_EFFECT_HELP_COUNT; i++) {
        const CliEffectHelp *e = &CLI_EFFECT_HELP[i];
        CHECK(e->about[0] != '\0');
        for (size_t k = 0; k < e->n_opts; k++) {
            CHECK(e->opts[k].help[0] != '\0');
            CHECK(!e->opts[k].def || !strstr(e->opts[k].def, "[default"));
        }
    }
    for (size_t i = 0; i < CLI_ROOT_HELP_COUNT; i++) {
        CHECK(CLI_ROOT_HELP[i].help[0] != '\0');
    }

    // Lookups resolve by name and miss cleanly.
    const CliEffectHelp *beams = cli_help_effect("beams");
    CHECK(beams != NULL);
    CHECK(cli_help_effect("not-an-effect") == NULL);
    const CliOptHelp *delay = beams ? cli_help_option(beams->opts, beams->n_opts, "beam-delay") : NULL;
    CHECK(delay != NULL && delay->def && strcmp(delay->def, "6") == 0);
    CHECK(beams && cli_help_option(beams->opts, beams->n_opts, "no-such-option") == NULL);
    return test_summary("test_cli_help");
}
