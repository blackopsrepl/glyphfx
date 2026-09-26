#!/usr/bin/env python3
"""Write README.md from the effect metadata plus the benchmark numbers.

Run after tools/demo/render_effects.py so the GIFs referenced here exist.
"""
import os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# (name, description) in the reference's order; descriptions match the CLI help.
EFFECTS = [
    ("beams", "Create beams which travel over the canvas illuminating the characters behind them."),
    ("binarypath", "Binary representations of each character move towards the home coordinate of the character."),
    ("blackhole", "Characters are consumed by a black hole and explode outwards."),
    ("bouncyballs", "Characters are bouncy balls falling from the top of the canvas."),
    ("bubbles", "Characters are formed into bubbles that float down and pop."),
    ("burn", "Burns vertically in the canvas."),
    ("colorshift", "Display a gradient that shifts colors across the terminal."),
    ("crumble", "Characters lose color and crumble into dust, vacuumed up, and reformed."),
    ("decrypt", "Display a movie style decryption effect."),
    ("errorcorrect", "Some characters start in the wrong position and are corrected in sequence."),
    ("expand", "Expands the text from a single point."),
    ("fireworks", "Characters launch and explode like fireworks and fall into place."),
    ("highlight", "Run a specular highlight across the text."),
    ("laseretch", "A laser etches characters onto the terminal."),
    ("matrix", "Matrix digital rain effect."),
    ("middleout", "Text expands in a single row or column in the middle of the canvas then out."),
    ("orbittingvolley", "Four launchers orbit the canvas firing volleys of characters inward to build the input text from the center out."),
    ("overflow", "Input text overflows and scrolls the terminal in a random order until eventually appearing ordered."),
    ("pour", "Pours the characters into position from the given direction."),
    ("print", "Lines are printed one at a time following a print head. Print head performs line feed, carriage return."),
    ("rain", "Rain characters from the top of the canvas."),
    ("randomsequence", "Prints the input data in a random sequence."),
    ("rings", "Characters are dispersed and form into spinning rings."),
    ("scattered", "Text is scattered across the canvas and moves into position."),
    ("slice", "Slices the input in half and slides it into place from opposite directions."),
    ("slide", "Slide characters into view from outside the terminal."),
    ("smoke", "Smoke floods the canvas colorizing any characters it crosses."),
    ("spotlights", "Spotlights search the text area, illuminating characters, before converging in the center and expanding."),
    ("spray", "Draws the characters spawning at varying rates from a single point."),
    ("swarm", "Characters are grouped into swarms and move around the terminal before settling into position."),
    ("sweep", "Sweep across the canvas to reveal uncolored text, reverse sweep to color the text."),
    ("synthgrid", "Create a grid which fills with characters dissolving into the final text."),
    ("thunderstorm", "Create a thunderstorm in the terminal."),
    ("unstable", "Spawn characters jumbled, explode them to the edge of the canvas, then reassemble them in the correct layout."),
    ("vhstape", "Lines of characters glitch left and right and lose detail like an old VHS tape."),
    ("waves", "Waves travel across the terminal leaving behind the characters."),
    ("wipe", "Wipes the text across the terminal to reveal characters."),
]


def effects_table():
    rows = []
    for i in range(0, len(EFFECTS), 2):
        cells = []
        for name, desc in EFFECTS[i:i + 2]:
            cells.append(f"| <b>{name}</b><br><img src=\"docs/effects/{name}.gif\" width=\"400\" alt=\"{name}\"><br><sub>{desc}</sub> ")
        while len(cells) < 2:
            cells.append("| ")
        rows.append("".join(cells) + "|")
    return "\n".join(rows)


README = f"""# glyphfx

Terminal text effects as a single C17 binary. Pipe text in, pick an effect:

```sh
ls -la | glyphfx decrypt
cat banner.txt | glyphfx beams
fortune | glyphfx --random-effect
git log --oneline -10 | glyphfx matrix
```

<img src="docs/effects/hero.gif" width="588" alt="the decrypt effect resolving the glyphfx banner">

## Why C

The effects are a shell toy that lives in a prompt pipeline, so startup and
throughput matter. glyphfx links nothing but libc and libm — no interpreter, no
import step, no third-party libraries — and starts in well under a
millisecond. `make` builds one binary.

## The effects

All 37, each with a full option surface (`glyphfx <effect> --help`).

|     |     |
|:---:|:---:|
{effects_table()}

## Benchmarks

Startup (median of 300 runs of `glyphfx --version`): **0.36 ms**. On a
160&times;40 canvas with pacing disabled (`--frame-rate 0`), best of three:

| effect | frames | ms/frame | fps |
|---|---:|---:|---:|
| blackhole | 300 | 0.020 | 50,148 |
| slide | 110 | 0.046 | 21,656 |
| waves | 300 | 0.061 | 16,291 |
| matrix | 300 | 0.086 | 11,599 |
| rings | 300 | 0.153 | 6,534 |
| beams | 300 | 0.782 | 1,279 |

Reproduce with `python3 tools/tests/bench.py`.

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

## Credit

glyphfx is a port of [ttfx](https://github.com/omacom/ttfx), which is a port of
[TerminalTextEffects](https://github.com/ChrisBuilds/terminaltexteffects) by
ChrisBuilds. The effect set, the animation engine, and the command-line
interface are that project's design; glyphfx implements them in C17 and matches
the ttfx binary byte for byte.

## License

MIT. See [LICENSE](LICENSE); attribution is in [NOTICE](NOTICE).
"""


def main():
    open(os.path.join(ROOT, "README.md"), "w").write(README)
    print("wrote README.md")


if __name__ == "__main__":
    main()
