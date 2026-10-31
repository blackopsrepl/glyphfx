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

The tempting alternative is hand-written assembly. ttfx tried it: an x86-64
engine landed in [omacom/ttfx#35](https://github.com/omacom/ttfx/pull/35) and was
removed again in [7444eea](https://github.com/omacom/ttfx/commit/7444eea), which
states that ttfx's portable Rust engine (`fx`) matches or beats it on every
effect; in the matrix below fx is faster on 36 of the 37 effects and asm on the other one. fx is the fastest port that ships, and glyphfx draws the
same conclusion:

- **The gains are algorithmic, not instruction-level.** fx credits
  structure-of-arrays character storage, interned visuals, an incremental cell
  grid, skipped idle ticks and batched writes for its speed — all expressible in
  C. fx is glyphfx's performance target, chased by transcribing that design into
  portable C rather than re-deriving it.
- **Assembly ties the fast path to one ISA.** The ttfx assembly engine needed
  NASM and per-tier x86-64 builds; everywhere else it declined and ran the old
  code. Portable C is the same program on every CPU.
- **A byte-exact target makes a second implementation a liability.** Every
  effect must reproduce ttfx bit for bit, including banker's rounding and libm
  results; a hand-tuned assembly engine doubles the surface where the two can
  silently diverge.

> **Why not assembly?** There is no frame budget to win. glyphfx paces itself at
> 60 fps and hands one batched frame to the terminal per tick; the
> thousands-of-fps figures are a benchmark, not the product. Data layout and
> skipped work are what make the benchmark faster, and that is portable C work.

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

## glyphfx originals

Four effects that ttfx does not have, from the SolverForge demoscene
productions. They follow the same contract (text in, the text in place at the
end; `burnout` is the one exit effect) but sit outside byte parity:
`--random-effect` draws them only when they are named in `--include-effects`,
and `make originals` holds them to determinism, golden streams and a
final-frame check instead. Details in [docs/ORIGINALS.md](docs/ORIGINALS.md).

|     |     |
|:---:|:---:|
| <b>bannerwave</b><br><img src="docs/effects/bannerwave.gif" width="400" alt="bannerwave"><br><sub>A bright front reveals the text while every column rides a travelling sine, then the text lies flat.</sub> | <b>burnout</b><br><img src="docs/effects/burnout.gif" width="400" alt="burnout"><br><sub>Exit effect: the text holds, then burns away through ember colours, shades and rising sparks.</sub> |
| <b>comet</b><br><img src="docs/effects/comet.gif" width="400" alt="comet"><br><sub>Characters fly arcs from the bottom of the canvas on fading trails, land bright and cool into place.</sub> | <b>copper</b><br><img src="docs/effects/copper.gif" width="400" alt="copper"><br><sub>Copper bars bounce behind dimmed text, lighting whole rows, then a sweep settles the final colours.</sub> |

## Benchmarks

Startup is under a millisecond (about 0.3 ms for `glyphfx --version`). The full
37-effect matrix against ttfx's engines: `fx`, the fastest port and glyphfx's
target; the removed assembly engine, for the record; and the Rust 0.3.3 engine
that is glyphfx's byte-parity oracle:

Best-of-N total run time at 200&times;50 with pacing disabled (`--frame-rate 0`), seed 1, on `x86_64:12th Gen Intel(R) Core(TM) i5-12400`. Speedups above 1.00&times; mean glyphfx is faster.

| effect | glyphfx | ttfx (fx) | vs fx | ttfx (asm) | vs asm | ttfx (Rust) | vs Rust |
|---|---:|---:|---:|---:|---:|---:|---:|
| beams | 74.3 ms | 18.7 ms | **0.25&times;** | 25.6 ms | **0.34&times;** | 320.8 ms | 4.32&times; |
| binarypath | 774.9 ms | 166.9 ms | **0.22&times;** | 355.4 ms | **0.46&times;** | 1512.1 ms | 1.95&times; |
| blackhole | 296.0 ms | 98.3 ms | **0.33&times;** | 116.6 ms | **0.39&times;** | 784.1 ms | 2.65&times; |
| bouncyballs | 121.9 ms | 50.8 ms | **0.42&times;** | 64.3 ms | **0.53&times;** | 527.5 ms | 4.33&times; |
| bubbles | 209.6 ms | 73.9 ms | **0.35&times;** | 94.2 ms | **0.45&times;** | 799.7 ms | 3.82&times; |
| burn | 79.8 ms | 34.3 ms | **0.43&times;** | 52.7 ms | **0.66&times;** | 598.9 ms | 7.51&times; |
| colorshift | 80.4 ms | 36.7 ms | **0.46&times;** | 33.1 ms | **0.41&times;** | 644.1 ms | 8.01&times; |
| crumble | 203.3 ms | 81.2 ms | **0.40&times;** | 107.7 ms | **0.53&times;** | 493.0 ms | 2.42&times; |
| decrypt | 86.4 ms | 27.4 ms | **0.32&times;** | 33.5 ms | **0.39&times;** | 711.5 ms | 8.23&times; |
| errorcorrect | 49.5 ms | 29.1 ms | **0.59&times;** | 40.6 ms | **0.82&times;** | 539.9 ms | 10.91&times; |
| expand | 82.4 ms | 30.3 ms | **0.37&times;** | 34.6 ms | **0.42&times;** | 194.9 ms | 2.37&times; |
| fireworks | 309.0 ms | 108.3 ms | **0.35&times;** | 126.6 ms | **0.41&times;** | 603.5 ms | 1.95&times; |
| highlight | 17.5 ms | 5.2 ms | **0.30&times;** | 7.1 ms | **0.41&times;** | 69.8 ms | 3.99&times; |
| laseretch | 151.1 ms | 102.9 ms | **0.68&times;** | 105.3 ms | **0.70&times;** | 912.8 ms | 6.04&times; |
| matrix | 91.6 ms | 50.6 ms | **0.55&times;** | 56.7 ms | **0.62&times;** | 260.9 ms | 2.85&times; |
| middleout | 71.1 ms | 16.5 ms | **0.23&times;** | 20.0 ms | **0.28&times;** | 142.2 ms | 2.00&times; |
| orbittingvolley | 65.9 ms | 26.3 ms | **0.40&times;** | 32.6 ms | **0.49&times;** | 105.7 ms | 1.60&times; |
| overflow | 79.8 ms | 22.8 ms | **0.29&times;** | 23.3 ms | **0.29&times;** | 156.8 ms | 1.96&times; |
| pour | 72.3 ms | 31.4 ms | **0.43&times;** | 37.6 ms | **0.52&times;** | 394.0 ms | 5.45&times; |
| print | 36.5 ms | 13.0 ms | **0.36&times;** | 14.7 ms | **0.40&times;** | 566.6 ms | 15.52&times; |
| rain | 92.5 ms | 34.2 ms | **0.37&times;** | 44.7 ms | **0.48&times;** | 306.1 ms | 3.31&times; |
| randomsequence | 18.3 ms | 6.2 ms | **0.34&times;** | 8.2 ms | **0.45&times;** | 74.4 ms | 4.07&times; |
| rings | 785.7 ms | 156.4 ms | **0.20&times;** | 201.8 ms | **0.26&times;** | 926.7 ms | 1.18&times; |
| scattered | 127.8 ms | 42.0 ms | **0.33&times;** | 49.4 ms | **0.39&times;** | 239.8 ms | 1.88&times; |
| slice | 57.5 ms | 15.9 ms | **0.28&times;** | 18.7 ms | **0.33&times;** | 107.3 ms | 1.87&times; |
| slide | 67.6 ms | 24.7 ms | **0.37&times;** | 29.1 ms | **0.43&times;** | 132.3 ms | 1.96&times; |
| smoke | 41.4 ms | 13.6 ms | **0.33&times;** | 21.7 ms | **0.52&times;** | 279.3 ms | 6.75&times; |
| spotlights | 201.0 ms | 49.0 ms | **0.24&times;** | 49.1 ms | **0.24&times;** | 437.7 ms | 2.18&times; |
| spray | 115.0 ms | 47.7 ms | **0.41&times;** | 55.2 ms | **0.48&times;** | 221.6 ms | 1.93&times; |
| swarm | 735.7 ms | 167.8 ms | **0.23&times;** | 194.4 ms | **0.26&times;** | 1044.8 ms | 1.42&times; |
| sweep | 19.9 ms | 7.5 ms | **0.38&times;** | 10.5 ms | **0.53&times;** | 104.2 ms | 5.24&times; |
| synthgrid | 35.2 ms | 10.6 ms | **0.30&times;** | 12.8 ms | **0.36&times;** | 170.7 ms | 4.85&times; |
| thunderstorm | 52.1 ms | 26.6 ms | **0.51&times;** | 36.4 ms | **0.70&times;** | 522.6 ms | 10.03&times; |
| unstable | 178.9 ms | 49.8 ms | **0.28&times;** | 59.3 ms | **0.33&times;** | 309.4 ms | 1.73&times; |
| vhstape | 155.7 ms | 42.9 ms | **0.28&times;** | 57.9 ms | **0.37&times;** | 532.9 ms | 3.42&times; |
| waves | 84.9 ms | 32.1 ms | **0.38&times;** | 36.9 ms | **0.43&times;** | 902.4 ms | 10.63&times; |
| wipe | 18.5 ms | 5.3 ms | **0.29&times;** | 7.1 ms | **0.38&times;** | 66.5 ms | 3.59&times; |

Reproduce with `python3 tools/tests/matrix.py` (`GLYPHFX_TTFX_FX` names a ttfx >= 0.5 binary for the fx column, `GLYPHFX_TTFX_ASM` a build of ttfx 73c7a44 for the asm column), and `make perf` to gate against regression.

## Usage

```
<producer> | glyphfx [terminal options] <effect> [effect options]

glyphfx --help                 # the 37 effects, the 4 originals, and the terminal options
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

The four originals have no oracle to match; `make originals` checks that they
are deterministic, match their recorded golden streams, and end on the right
final frame.

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
