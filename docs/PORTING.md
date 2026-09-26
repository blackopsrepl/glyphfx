# Porting an effect to C

glyphfx is a byte-exact parity port of ttfx. Each effect is one C file plus a
header, a registry entry, and parity cases. The reference is
`reference/src/effects/<name>.rs`; the target is parity with the oracle binary.

## Definition of done

```
make
tools/parity/run_effects.sh <name>      # must report "0 failed"
```

A new effect file must:
- define `<name>_entry` (a `const EffectEntry`) with a strong symbol;
- define the config struct, option spec table, defaults, free_config, make;
- be self-contained: create only `src/effects/<name>.c`, `src/effects/<name>.h`,
  and `tools/parity/cases/<name>.txt`. Do not edit the registry, cli, engine,
  or another effect. The registry declares every `<name>_entry` weak, so a
  partial tree still links.

## File skeleton

```c
#include "effects/<name>.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "effects/registry.h"
#include "engine/animation.h"
#include "engine/ctx.h"
#include "engine/terminal.h"
/* plus engine/motion.h, engine/particles.h, utils/spanning_tree.h,
   utils/geometry.h, utils/easing.h, utils/sequence_easer.h as needed */

typedef struct { /* config fields, matching the Rust struct */ } <Name>Config;

void <name>_config_defaults(void *cfg);   /* set every default, including lists */
void <name>_free_config(void *cfg);       /* free owned list/string fields */
Effect *<name>_make(const void *cfg);

/* effect state */
typedef struct { <Name>Config config; /* ... */ } <Name>;

static int <name>_build(Effect *self, EngineCtx *ctx);        /* 0 ok, -1 error */
static char *<name>_next_frame(Effect *self, EngineCtx *ctx); /* malloc'd or NULL */
static void <name>_destroy(Effect *self);
static void <name>_callback(Effect *self, EngineCtx *ctx, CharId ch, const EffectCallback *cb); /* if any */

static const EffectOps OPS = { <name>_build, <name>_next_frame, <name>_destroy, NULL /* or callback */ };

Effect *<name>_make(const void *cfg) {
    <Name> *st = calloc(1, sizeof(<Name>));
    Effect *e = calloc(1, sizeof(Effect));
    st->config = *(const <Name>Config *)cfg;
    e->ops = &OPS;
    e->state = st;
    return e;
}

const EffectEntry <name>_entry = {
    "<name>",
    (const EffOptSpec[]){ EF_SPEC("option-name", 0, EF_KIND, offsetof(<Name>Config, field)), ... },
    N_OPTIONS,
    sizeof(<Name>Config),
    <name>_config_defaults,
    <name>_free_config,
    <name>_make,
};
```

`merge_effect_make` is not a thing: the effect state copies the config by
value. If the config has `char *` symbol fields, `free_config` frees them.

## Option kinds (see src/cli.h)

`EF_FLAG EF_INT EF_POS_INT EF_NONNEG_INT EF_FLOAT_POS EF_FLOAT_NONNEG
EF_RATIO_NONNEG EF_RATIO_POS EF_STRING EF_SYMBOL EF_COLOR EF_DIRECTION
EF_EASING EF_CHAR_SORT EF_CHAR_GROUP EF_COLOR_LIST EF_INT_LIST EF_INT_RANGE
EF_FLOAT_RANGE EF_CUSTOM`

- `EF_COLOR_LIST` / `EF_INT_LIST` fields are `ColorList` / `IntList`.
- `EF_INT_RANGE` / `EF_FLOAT_RANGE` fields are `IntRange` / `FloatRange`.
- `EF_SYMBOL` field is `char *` (owned; free it).
- `EF_EASING` field is `Easing` (by value). Reference defaults: `easing_parse("in_out_circ", &cfg->x)`.
- `EF_CUSTOM` calls a per-effect `int parse_<x>(const char *value, void *dst)`.
  Set the spec's `.custom` field by using a named array instead of the
  designated `EF_SPEC` macro, e.g. `{ "etch-pattern", 0, EF_CUSTOM,
  offsetof(Cfg, p), parse_etch_pattern }`.

## Engine API cheat-sheet (C names for the Rust calls)

- Arena: `ctx->terminal.arena.items[id]` is an `EffectCharacter *`.
- Character: `.input_symbol .input_coord .motion .animation .layer
  .is_visible .uses_input_preexisting_colors .event_handler .links/.links_len
  .north/.east/.south/.west`.
- Motion: `motion_new_path(&ch->motion, speed, has_ease, ease, has_layer,
  layer, hold, loop, id, &out_id)`; `path_new_waypoint(path, coord, bezier,
  bezier_len, id, &out_wp)`; `path_query_waypoint`; `om_get(&ch->motion.paths,
  id)`; `motion_deactivate_path`.
- Animation: `animation_new_scene(&ch->animation, looping, has_sync, sync,
  has_ease, ease, id, uses_input_preexisting_colors)`; scene via
  `om_get(&ch->animation.scenes, id)`; `scene_add_frame(scene, symbol,
  duration, &VisualParams)`; `scene_apply_gradient_to_symbols(scene, symbols,
  n, duration, fg, bg)`; `animation_set_appearance`.
- Engine: `engine_activate_scene/path(ctx, self, id, idstr)`,
  `engine_deactivate_scene`, `engine_register_event(ctx, id, event, &CallerKey,
  &EventAction)`, `engine_chain_paths`, `engine_update`, `engine_frame`,
  `engine_caller_from_waypoint` (then `caller_key_free`), `ac_insert`,
  `ac_is_empty`, `ac_extend`, `ac_clear`.
- Terminal: `terminal_get_characters(&ctx->terminal, &ctx->rng, filter, sort,
  &n)`, `terminal_get_characters_grouped`, `terminal_get_input_colors`,
  `terminal_set_character_visibility`, `terminal_add_character`,
  `terminal_get_character_by_input_coord`.
- Canvas: `canvas_random_coord`, `canvas_random_column`, `canvas_random_row`;
  fields `top right bottom left center center_row center_column text_*`.
- Graphics: `gradient_new`, `gradient_with_steps`,
  `gradient_build_coordinate_color_mapping`, `coordcolormap_get`,
  `gradient_get_color_at_fraction`, `shift_color_towards`,
  `color_adjust_brightness`, `random_color`.
- Easing: `easing_ease(&e, p)`.
- RNG: `rng_randint rng_randrange rng_choice_index rng_uniform rng_random
  rng_shuffle`. `rng_choice_index(rng, n)` replaces `rng.choice(&slice)`.

## Behavioral rules that must not drift

1. Transcribe arithmetic verbatim (expression order, `pow(x, 2.0)` not
   multiplication). Quantization uses banker's rounding via `py_round_half_even`.
2. Match RNG call order exactly. Where the reference builds four candidate
   coords before `choice`, do the same.
3. Never call `ctx->terminal.arena.items[...]` across an event emission or a
   call that can add characters/particles — re-fetch after. Paths are heap
   pointers and stable unless explicitly cleared.
4. `existing_color_handling == EXISTING_COLOR_DYNAMIC` uses the character's
   input colors; otherwise a final gradient mapping is used. `ALWAYS` is
   handled inside scenes via `uses_input_preexisting_colors`.
5. Insertion order is behavior. Use `OrdMap` for insertion-ordered id->value
   maps; iterate `active_characters` only through `ac_snapshot` (ascending id).

## Parity cases

`tools/parity/cases/<name>.txt` lines are `<seed> | <terminal opts> | <effect
opts>` (terminal opts precede the effect name). Include several seeds, a
couple of inputs' worth of configurations, and 2+ non-default effect options.
Use `GLYPHFX_MAX_FRAMES` if an effect needs more than 60 frames to run.
`tools/parity/run_effects.sh <name>` runs it and reports the first divergence.
