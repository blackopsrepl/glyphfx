# glyphfx originals

glyphfx is a byte-exact port of ttfx, and `docs/PORTING.md` governs the 37
effects that make up that port. The originals are effects that glyphfx ships
and ttfx does not. They come from the SolverForge demoscene productions, where
they were first written as textmode demo effects, and they obey the same
contract as every other effect: text goes in, an animation plays, and the
entrance effects end with the text in its home position and final colours.

Four effects make up the set:

| effect | kind | what it does |
|---|---|---|
| `comet` | entrance | Characters launch from the bottom centre of the canvas, fly an arc on a fading trail, land bright and cool to their final colour. |
| `copper` | entrance | Amiga-style copper bars bounce behind dimmed text, lighting whole rows of the canvas background, then a sweep settles the text into its final colours. |
| `bannerwave` | entrance | A bright front reveals the text while every column rides a travelling sine like a flag in wind, and the wave dies away until the text lies flat. |
| `burnout` | exit | The text holds in its final colours, then every character ignites, decays through ember colours and block shades, and throws off sparks until the canvas is empty. |

## Where they differ from the port

The parity effects are built from the engine's scenes, paths and events,
because that is how ttfx builds them and the byte streams must match. The
originals have no stream to match, so each one computes every character's
symbol, colours, position and layer as a pure function of the frame number
and sets them directly (`animation_set_appearance`, `motion_set_coordinate`,
visibility and layer). The shared pieces live in `src/effects/originals.c`:
final colours that follow the same gradient and `--existing-color-handling`
rules as the port, and colour mixes snapped to 16 steps so the interned visual
pool stays bounded however long an effect runs.

They stay out of the parity contract in three concrete ways:

- **Registry order.** `GLYPHFX_ORIGINAL_EFFECT_NAMES` registers them after the
  ttfx set, so every ttfx effect keeps its index.
- **`--random-effect`.** The random draw covers the ttfx set only, unless the
  originals are named in `--include-effects`. A seeded random run therefore
  picks exactly the effect ttfx picks.
- **Help.** `glyphfx --help` lists them under their own heading.

## Options

Every option has a default, and `glyphfx <effect> --help` lists them. Frame
counts are frames, not seconds, which is the convention across glyphfx.

**comet**

| option | default | meaning |
|---|---|---|
| `--launch-order` | `column_left_to_right` | character grouping that sets the launch sequence |
| `--launch-delay` | `2` | frames between consecutive groups |
| `--launch-jitter` | `10` | random extra frames added to each launch |
| `--flight-frames` | `50` | frames each flight takes |
| `--flight-ease` | `in_out_sine` | easing along the arc |
| `--arc-height` | `3-8` | range of rows each arc bows upward |
| `--trail-length` | `10` | trail points behind each comet (0 disables the trail) |
| `--trail-symbols` | `▓ ▒ ░ ·` | symbols from the head of the trail to its tail |
| `--comet-color` | `ffffff` | colour of the flying head and of the landing flash |
| `--cooldown-frames` | `20` | frames a landed character takes to cool to its final colour |

**copper**

| option | default | meaning |
|---|---|---|
| `--bar-colors` | `82fb9c 7cf8f7 8a7cf8` | one bar per colour |
| `--bar-height` | `5` | rows each bar spans |
| `--bounce-frames` | `180` | frames the bars bounce; they fade over the last quarter |
| `--settle-frames` | `80` | frames the settling sweep takes to cross the canvas |
| `--unlit-color` | `3c3f5a` | text colour before the sweep reaches it |
| `--sweep-color` | `e8fff0` | colour of the settling sweep |

**bannerwave**

| option | default | meaning |
|---|---|---|
| `--wave-amplitude` | `1.6` | peak displacement in rows |
| `--wave-length` | `63` | columns per wave cycle; keep it longer than a letter so block glyphs stay whole |
| `--wave-period` | `75` | frames per wave cycle |
| `--reveal-frames` | `66` | frames the reveal front takes to cross the text |
| `--wave-frames` | `156` | frames before the wave starts to die away |
| `--settle-frames` | `90` | frames the wave takes to die away |
| `--highlight-color` | `ffffff` | colour of the crests and of the reveal front |

**burnout**

| option | default | meaning |
|---|---|---|
| `--hold-frames` | `36` | frames the text holds before the first character ignites |
| `--burn-direction` | `column_left_to_right` | character grouping the fire spreads along |
| `--burn-spread` | `72` | frames the fire takes to cross the text |
| `--burn-jitter` | `72` | random extra frames before each character ignites |
| `--burn-frames` | `33` | frames each character takes to burn away |
| `--burn-symbols` | `▓ ▒ ░ ·` | decay symbols after ignition |
| `--ember-colors` | `ffffff ffd88a ff7a00 7a1a00` | colour ramp from ignition to ash |
| `--spark-ratio` | `0.35` | share of characters that throw a spark |
| `--spark-frames` | `54` | frames a spark rises before it dies |

Every effect also takes `--final-gradient-stops` (default
`82fb9c 7cf8f7 8a7cf8`), `--final-gradient-steps` (`12`) and
`--final-gradient-direction` (`horizontal`). For `burnout` the gradient colours
the text while it holds.

## Verification

With no oracle to compare against, `make originals` (`tools/tests/originals.py`)
checks each effect another way:

- **Determinism.** Every case runs twice and the two `--parity-dump` streams
  must be byte-identical.
- **Goldens.** Each stream's sha256 and frame count must match
  `tests/fixtures/originals_goldens.tsv`. The goldens are pinned to Linux/glibc,
  as the parity suite is, and `--no-goldens` skips them elsewhere.
- **Final frame.** The entrance effects must end with exactly the text that
  `wipe` leaves on the same canvas, and `burnout` must end with an empty one.
- **Surface.** The cases cover colour modes (`--xterm-colors`, `--no-color`,
  dynamic and always colour handling on ANSI input), a single character, text
  larger than the canvas, and non-default options. Invalid option values must
  exit 2 with the message on stderr.

`tests/test_originals.c` covers the registry placement and the colour helpers.
`make perf` gates all four effects for speed and output alongside the port.
After an intended change to an original, refresh the goldens with
`tools/tests/originals.py --update` and say why in the commit.
