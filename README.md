# glyphfx

Terminal text effects as a single C17 binary. Pipe text in, pick an effect:

```sh
ls -la | glyphfx decrypt
cat banner.txt | glyphfx beams
fortune | glyphfx --random-effect
git log --oneline -10 | glyphfx matrix
```

glyphfx animates stdin and writes the frames to the terminal. It has no
runtime dependencies beyond libc and libm.

## Why C

The effects are a shell toy that lives in a prompt pipeline, so startup and
throughput matter. glyphfx links nothing but libc and libm: no interpreter, no
import step, no third-party libraries. `make` builds one binary.

## The effects

All 37, each with a full option surface (`glyphfx <effect> --help`).

| effect | what it does |
|---|---|
| <b>beams</b> | Create beams which travel over the canvas illuminating the characters behind them. |
| <b>binarypath</b> | Binary representations of each character move towards the home coordinate of the character. |
| <b>blackhole</b> | Characters are consumed by a black hole and explode outwards. |
| <b>bouncyballs</b> | Characters are bouncy balls falling from the top of the canvas. |
| <b>bubbles</b> | Characters are formed into bubbles that float down and pop. |
| <b>burn</b> | Burns vertically in the canvas. |
| <b>colorshift</b> | Display a gradient that shifts colors across the terminal. |
| <b>crumble</b> | Characters lose color and crumble into dust, vacuumed up, and reformed. |
| <b>decrypt</b> | Display a movie style decryption effect. |
| <b>errorcorrect</b> | Some characters start in the wrong position and are corrected in sequence. |
| <b>expand</b> | Expands the text from a single point. |
| <b>fireworks</b> | Characters launch and explode like fireworks and fall into place. |
| <b>highlight</b> | Run a specular highlight across the text. |
| <b>laseretch</b> | A laser etches characters onto the terminal. |
| <b>matrix</b> | Matrix digital rain effect. |
| <b>middleout</b> | Text expands in a single row or column in the middle of the canvas then out. |
| <b>orbittingvolley</b> | Four launchers orbit the canvas firing volleys of characters inward to build the input text from the center out. |
| <b>overflow</b> | Input text overflows and scrolls the terminal in a random order until eventually appearing ordered. |
| <b>pour</b> | Pours the characters into position from the given direction. |
| <b>print</b> | Lines are printed one at a time following a print head. Print head performs line feed, carriage return. |
| <b>rain</b> | Rain characters from the top of the canvas. |
| <b>randomsequence</b> | Prints the input data in a random sequence. |
| <b>rings</b> | Characters are dispersed and form into spinning rings. |
| <b>scattered</b> | Text is scattered across the canvas and moves into position. |
| <b>slice</b> | Slices the input in half and slides it into place from opposite directions. |
| <b>slide</b> | Slide characters into view from outside the terminal. |
| <b>smoke</b> | Smoke floods the canvas colorizing any characters it crosses. |
| <b>spotlights</b> | Spotlights search the text area, illuminating characters, before converging in the center and expanding. |
| <b>spray</b> | Draws the characters spawning at varying rates from a single point. |
| <b>swarm</b> | Characters are grouped into swarms and move around the terminal before settling into position. |
| <b>sweep</b> | Sweep across the canvas to reveal uncolored text, reverse sweep to color the text. |
| <b>synthgrid</b> | Create a grid which fills with characters dissolving into the final text. |
| <b>thunderstorm</b> | Create a thunderstorm in the terminal. |
| <b>unstable</b> | Spawn characters jumbled, explode them to the edge of the canvas, then reassemble them in the correct layout. |
| <b>vhstape</b> | Lines of characters glitch left and right and lose detail like an old VHS tape. |
| <b>waves</b> | Waves travel across the terminal leaving behind the characters. |
| <b>wipe</b> | Wipes the text across the terminal to reveal characters. |

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

Given the same input, options, and seed, glyphfx emits byte-identical frames
and a byte-identical terminal stream to ttfx. It is verified mechanically
against the ttfx binary, not by eye:

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

## License

MIT. See [LICENSE](LICENSE); attribution is in [NOTICE](NOTICE).
