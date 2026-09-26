# glyphfx

<p align="center">
  <img src="docs/images/glyphfx-mascot.png" width="360" alt="The GlyphFX Cursor Gecko mascot, with terminal glyphs resolving along its C-shaped tail">
</p>

Terminal text effects as a single C17 binary. Pipe text in, pick an effect:

```sh
ls -la | glyphfx decrypt
cat banner.txt | glyphfx beams
fortune | glyphfx --random-effect
git log --oneline -10 | glyphfx matrix
```

<p align="center">
  <img src="docs/effects/hero.gif" width="588" alt="the wipe effect resolving the glyphfx banner">
</p>

## Why C

The effects are a shell toy that lives in a prompt pipeline, so startup and
throughput matter. glyphfx links nothing but libc and libm — no interpreter, no
import step, no third-party libraries — and starts in well under a millisecond.

The tempting alternative is to hand-write the hot paths in x86-64 assembly, as
ttfx experimented with in [an open pull request](https://github.com/omacom/ttfx/pull/35).
glyphfx does not, deliberately:

- **The measured gains were algorithmic, not instruction-level.** That PR's own
  notes credit structure-of-arrays character storage, pooled visuals, an
  incremental cell grid, batched writes, and a faster RNG for most of its
  speedup — all expressible in C. glyphfx has closed the gap with the Rust
  engine across the matrix; the open target is the assembly engine, chased by
  transcribing its optimized logic (soa layout, pooling, batching) into
  portable C rather than re-deriving wins.
- **Assembly ties the fast path to one ISA.** That engine only runs on
  x86-64-v4 (AVX-512), which Intel disabled on consumer parts after 11th gen,
  AMD shipped only from Zen 4, and no ARM or Apple silicon has. Elsewhere it
  declines and you get the old code. Portable C is the same speed everywhere.
- **A byte-exact target makes a second implementation a liability.** Every
  effect must reproduce ttfx bit for bit, including banker's rounding and libm
  results; a hand-tuned assembly engine doubles the surface where the two can
  silently diverge.

> **Why not assembly?** There is no frame budget to win. glyphfx paces itself at
> 60 fps and hands one batched frame to the terminal per tick; the
> tens-of-thousands-of-fps figures are a benchmark, not the product. ttfx's own
> AVX-512 experiment credits data layout and batched writes for its speedup, not
> instruction selection — and that is portable C work. Hand-written SIMD would
> accelerate a number nobody watches while trading away the single binary that
> stays byte-exact with the oracle.

The result builds anywhere with a C compiler, starts in about 0.3 ms, and
renders every effect at thousands to tens of thousands of frames per second
with pacing disabled.

## The effects

All 37, each with a full option surface (`glyphfx <effect> --help`).

|     |     |
|:---:|:---:|
| <b>beams</b><br><img src="docs/effects/beams.gif" width="400" alt="beams"><br><sub>Create beams which travel over the canvas illuminating the characters behind them.</sub> | <b>binarypath</b><br><img src="docs/effects/binarypath.gif" width="400" alt="binarypath"><br><sub>Binary representations of each character move towards the home coordinate of the character.</sub> |
| <b>blackhole</b><br><img src="docs/effects/blackhole.gif" width="400" alt="blackhole"><br><sub>Characters are consumed by a black hole and explode outwards.</sub> | <b>bouncyballs</b><br><img src="docs/effects/bouncyballs.gif" width="400" alt="bouncyballs"><br><sub>Characters are bouncy balls falling from the top of the canvas.</sub> |
| <b>bubbles</b><br><img src="docs/effects/bubbles.gif" width="400" alt="bubbles"><br><sub>Characters are formed into bubbles that float down and pop.</sub> | <b>burn</b><br><img src="docs/effects/burn.gif" width="400" alt="burn"><br><sub>Burns vertically in the canvas.</sub> |
| <b>colorshift</b><br><img src="docs/effects/colorshift.gif" width="400" alt="colorshift"><br><sub>Display a gradient that shifts colors across the terminal.</sub> | <b>crumble</b><br><img src="docs/effects/crumble.gif" width="400" alt="crumble"><br><sub>Characters lose color and crumble into dust, vacuumed up, and reformed.</sub> |
| <b>decrypt</b><br><img src="docs/effects/decrypt.gif" width="400" alt="decrypt"><br><sub>Display a movie style decryption effect.</sub> | <b>errorcorrect</b><br><img src="docs/effects/errorcorrect.gif" width="400" alt="errorcorrect"><br><sub>Some characters start in the wrong position and are corrected in sequence.</sub> |
| <b>expand</b><br><img src="docs/effects/expand.gif" width="400" alt="expand"><br><sub>Expands the text from a single point.</sub> | <b>fireworks</b><br><img src="docs/effects/fireworks.gif" width="400" alt="fireworks"><br><sub>Characters launch and explode like fireworks and fall into place.</sub> |
| <b>highlight</b><br><img src="docs/effects/highlight.gif" width="400" alt="highlight"><br><sub>Run a specular highlight across the text.</sub> | <b>laseretch</b><br><img src="docs/effects/laseretch.gif" width="400" alt="laseretch"><br><sub>A laser etches characters onto the terminal.</sub> |
| <b>matrix</b><br><img src="docs/effects/matrix.gif" width="400" alt="matrix"><br><sub>Matrix digital rain effect.</sub> | <b>middleout</b><br><img src="docs/effects/middleout.gif" width="400" alt="middleout"><br><sub>Text expands in a single row or column in the middle of the canvas then out.</sub> |
| <b>orbittingvolley</b><br><img src="docs/effects/orbittingvolley.gif" width="400" alt="orbittingvolley"><br><sub>Four launchers orbit the canvas firing volleys of characters inward to build the input text from the center out.</sub> | <b>overflow</b><br><img src="docs/effects/overflow.gif" width="400" alt="overflow"><br><sub>Input text overflows and scrolls the terminal in a random order until eventually appearing ordered.</sub> |
| <b>pour</b><br><img src="docs/effects/pour.gif" width="400" alt="pour"><br><sub>Pours the characters into position from the given direction.</sub> | <b>print</b><br><img src="docs/effects/print.gif" width="400" alt="print"><br><sub>Lines are printed one at a time following a print head. Print head performs line feed, carriage return.</sub> |
| <b>rain</b><br><img src="docs/effects/rain.gif" width="400" alt="rain"><br><sub>Rain characters from the top of the canvas.</sub> | <b>randomsequence</b><br><img src="docs/effects/randomsequence.gif" width="400" alt="randomsequence"><br><sub>Prints the input data in a random sequence.</sub> |
| <b>rings</b><br><img src="docs/effects/rings.gif" width="400" alt="rings"><br><sub>Characters are dispersed and form into spinning rings.</sub> | <b>scattered</b><br><img src="docs/effects/scattered.gif" width="400" alt="scattered"><br><sub>Text is scattered across the canvas and moves into position.</sub> |
| <b>slice</b><br><img src="docs/effects/slice.gif" width="400" alt="slice"><br><sub>Slices the input in half and slides it into place from opposite directions.</sub> | <b>slide</b><br><img src="docs/effects/slide.gif" width="400" alt="slide"><br><sub>Slide characters into view from outside the terminal.</sub> |
| <b>smoke</b><br><img src="docs/effects/smoke.gif" width="400" alt="smoke"><br><sub>Smoke floods the canvas colorizing any characters it crosses.</sub> | <b>spotlights</b><br><img src="docs/effects/spotlights.gif" width="400" alt="spotlights"><br><sub>Spotlights search the text area, illuminating characters, before converging in the center and expanding.</sub> |
| <b>spray</b><br><img src="docs/effects/spray.gif" width="400" alt="spray"><br><sub>Draws the characters spawning at varying rates from a single point.</sub> | <b>swarm</b><br><img src="docs/effects/swarm.gif" width="400" alt="swarm"><br><sub>Characters are grouped into swarms and move around the terminal before settling into position.</sub> |
| <b>sweep</b><br><img src="docs/effects/sweep.gif" width="400" alt="sweep"><br><sub>Sweep across the canvas to reveal uncolored text, reverse sweep to color the text.</sub> | <b>synthgrid</b><br><img src="docs/effects/synthgrid.gif" width="400" alt="synthgrid"><br><sub>Create a grid which fills with characters dissolving into the final text.</sub> |
| <b>thunderstorm</b><br><img src="docs/effects/thunderstorm.gif" width="400" alt="thunderstorm"><br><sub>Create a thunderstorm in the terminal.</sub> | <b>unstable</b><br><img src="docs/effects/unstable.gif" width="400" alt="unstable"><br><sub>Spawn characters jumbled, explode them to the edge of the canvas, then reassemble them in the correct layout.</sub> |
| <b>vhstape</b><br><img src="docs/effects/vhstape.gif" width="400" alt="vhstape"><br><sub>Lines of characters glitch left and right and lose detail like an old VHS tape.</sub> | <b>waves</b><br><img src="docs/effects/waves.gif" width="400" alt="waves"><br><sub>Waves travel across the terminal leaving behind the characters.</sub> |
| <b>wipe</b><br><img src="docs/effects/wipe.gif" width="400" alt="wipe"><br><sub>Wipes the text across the terminal to reveal characters.</sub> | |

## Benchmarks

Startup is under a millisecond (about 0.3 ms for `glyphfx --version`). The full
37-effect matrix against the ttfx assembly engine (the Rust port is shown for
provenance; glyphfx is on par with it across the matrix):

Best-of-N total run time at 200&times;50 with pacing disabled (`--frame-rate 0`), seed 1, on `x86_64:12th Gen Intel(R) Core(TM) i5-12400`. Speedups above 1.00&times; mean glyphfx is faster.

| effect | glyphfx | ttfx (asm) | vs asm | ttfx (Rust) | vs Rust |
|---|---:|---:|---:|---:|---:|
| beams | 264.6 ms | 31.8 ms | **0.12&times;** | 364.4 ms | 1.38&times; |
| binarypath | 1366.3 ms | 464.5 ms | **0.34&times;** | 1616.6 ms | 1.18&times; |
| blackhole | 815.5 ms | 164.4 ms | **0.20&times;** | 866.2 ms | 1.06&times; |
| bouncyballs | 615.0 ms | 114.1 ms | **0.19&times;** | 564.9 ms | 0.92&times; |
| bubbles | 815.7 ms | 114.0 ms | **0.14&times;** | 865.2 ms | 1.06&times; |
| burn | 564.9 ms | 63.9 ms | **0.11&times;** | 614.8 ms | 1.09&times; |
| colorshift | 564.7 ms | 64.2 ms | **0.11&times;** | 715.0 ms | 1.27&times; |
| crumble | 514.9 ms | 114.0 ms | **0.22&times;** | 514.7 ms | 1.00&times; |
| decrypt | 765.2 ms | 64.3 ms | **0.08&times;** | 766.1 ms | 1.00&times; |
| errorcorrect | 614.9 ms | 64.1 ms | **0.10&times;** | 565.0 ms | 0.92&times; |
| expand | 264.2 ms | 63.9 ms | **0.24&times;** | 264.3 ms | 1.00&times; |
| fireworks | 665.7 ms | 164.1 ms | **0.25&times;** | 614.9 ms | 0.92&times; |
| highlight | 63.8 ms | 15.9 ms | **0.25&times;** | 114.0 ms | 1.79&times; |
| laseretch | 965.8 ms | 113.9 ms | **0.12&times;** | 965.5 ms | 1.00&times; |
| matrix | 364.4 ms | 63.9 ms | **0.18&times;** | 314.4 ms | 0.86&times; |
| middleout | 164.1 ms | 31.8 ms | **0.19&times;** | 164.1 ms | 1.00&times; |
| orbittingvolley | 164.0 ms | 64.0 ms | **0.39&times;** | 114.1 ms | 0.70&times; |
| overflow | 264.3 ms | 31.8 ms | **0.12&times;** | 214.2 ms | 0.81&times; |
| pour | 464.7 ms | 63.9 ms | **0.14&times;** | 465.4 ms | 1.00&times; |
| print | 614.9 ms | 15.7 ms | **0.03&times;** | 615.0 ms | 1.00&times; |
| rain | 414.5 ms | 63.9 ms | **0.15&times;** | 364.4 ms | 0.88&times; |
| randomsequence | 63.8 ms | 7.8 ms | **0.12&times;** | 114.0 ms | 1.79&times; |
| rings | 1266.0 ms | 264.2 ms | **0.21&times;** | 1015.6 ms | 0.80&times; |
| scattered | 264.6 ms | 63.9 ms | **0.24&times;** | 264.7 ms | 1.00&times; |
| slice | 164.1 ms | 31.8 ms | **0.19&times;** | 164.1 ms | 1.00&times; |
| slide | 164.1 ms | 31.8 ms | **0.19&times;** | 164.1 ms | 1.00&times; |
| smoke | 264.3 ms | 31.9 ms | **0.12&times;** | 314.3 ms | 1.19&times; |
| spotlights | 464.6 ms | 114.0 ms | **0.25&times;** | 464.6 ms | 1.00&times; |
| spray | 264.4 ms | 64.1 ms | **0.24&times;** | 264.5 ms | 1.00&times; |
| swarm | 1216.2 ms | 264.2 ms | **0.22&times;** | 1065.7 ms | 0.88&times; |
| sweep | 113.9 ms | 15.7 ms | **0.14&times;** | 114.0 ms | 1.00&times; |
| synthgrid | 164.0 ms | 15.8 ms | **0.10&times;** | 214.1 ms | 1.31&times; |
| thunderstorm | 515.3 ms | 63.8 ms | **0.12&times;** | 615.0 ms | 1.19&times; |
| unstable | 314.4 ms | 114.1 ms | **0.36&times;** | 364.8 ms | 1.16&times; |
| vhstape | 514.6 ms | 114.0 ms | **0.22&times;** | 615.0 ms | 1.20&times; |
| waves | 614.9 ms | 64.0 ms | **0.10&times;** | 1015.6 ms | 1.65&times; |
| wipe | 64.1 ms | 7.7 ms | **0.12&times;** | 114.0 ms | 1.78&times; |

Reproduce with `python3 tools/tests/matrix.py` (and `make perf` to gate against regression).

## Usage

```
<producer> | glyphfx [terminal options] <effect> [effect options]

glyphfx --help                 # all 37 effects and the terminal options
glyphfx <effect> --help        # options for one effect
glyphfx --random-effect        # surprise me (--include-effects / --exclude-effects to filter)
glyphfx --print-completion bash|zsh
```

Terminal options (canvas size and anchoring, color handling, frame rate, text
wrapping) go before the effect name; effect options after it. Option names,
defaults, metavars, choices, negative-value handling, and nargs match ttfx, so
existing invocations work with the binary name swapped.

## Building

libc and libm only.

```sh
make            # build/glyphfx
make check      # unit tests (pure-function, geometry, gradient, RNG goldens)
make debug      # ASan/UBSan build
make install    # PREFIX=/usr/local by default
make static     # static link (build/glyphfx-static)
```

## Fidelity

Given the same input, options, and seed, glyphfx produces byte-identical frames
and a byte-identical terminal stream to ttfx, verified mechanically against the
ttfx binary rather than by eye:

```sh
make parity     # M0 input/canvas/anchoring option matrix
make effects    # every effect's frame stream across seeds and configs
tools/tests/cli_corpus.sh    # exit codes and stdout/stderr routing
tools/tests/tty_compare.py   # the full pty prep+frames+teardown stream
tools/tests/tty_signals.py   # SIGINT/SIGTERM/close/resize behavior
```

The oracle is a local ttfx checkout, fetched on demand and gitignored:

```sh
tools/parity/fetch_reference.sh
```

`make effects` expects `reference/target/release/ttfx` unless `GLYPHFX_TTFX` is
set. A single effect runs with `tools/parity/run_effects.sh <effect>`.

## Scope

Linux and macOS. Byte-exact comparison is pinned to Linux/glibc; builds and the
unit tests also run on macOS.

## Project facts

`repo_facts.db` is a [nisaba](https://github.com/blackopsrepl/nisaba) store: an
append-only, temporal key-value log of this project's decisions, benchmarks,
releases and verification results, committed on purpose so the claims in this
repository carry provenance rather than living in chat. Read it with the
`nisaba` CLI:

```sh
nisaba repo_facts.db canon project.release.latest    # current value of one key
nisaba repo_facts.db scan perf                        # everything under a prefix
nisaba repo_facts.db history perf.milestone.v1_0_0    # how a claim changed
```

The log is the source of truth; `repo_facts.db.idx`, if present, is a disposable
index cache and is not committed.

## Credit

glyphfx is a C port of [ttfx](https://github.com/omacom/ttfx), which is a Rust
port of [TerminalTextEffects](https://github.com/ChrisBuilds/terminaltexteffects)
by ChrisBuilds. The effect set, the animation engine, and the command-line
interface are TerminalTextEffects' design; ttfx is the reference glyphfx
matches byte for byte.

## License

MIT. See [LICENSE](LICENSE); attribution is in [NOTICE](NOTICE).
