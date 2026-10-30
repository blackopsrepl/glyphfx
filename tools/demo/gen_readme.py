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

# glyphfx originals: no ttfx counterpart, documented in docs/ORIGINALS.md.
ORIGINALS = [
    ("bannerwave", "A bright front reveals the text while every column rides a travelling sine, then the text lies flat."),
    ("burnout", "Exit effect: the text holds, then burns away through ember colours, shades and rising sparks."),
    ("comet", "Characters fly arcs from the bottom of the canvas on fading trails, land bright and cool into place."),
    ("copper", "Copper bars bounce behind dimmed text, lighting whole rows, then a sweep settles the final colours."),
]


def matrix_table():
    """Render the full performance matrix recorded by tools/tests/matrix.py."""
    path = os.path.join(ROOT, "docs/benchmarks/matrix.tsv")
    if not os.path.exists(path):
        return "_No matrix recorded; run `python3 tools/tests/matrix.py`._"
    machine = ""
    rows = []
    for line in open(path):
        if line.startswith("# machine "):
            machine = line.strip()[len("# machine "):]
        elif line.startswith("#") or not line.strip():
            continue
        parts = line.rstrip("\n").split("\t")
        if len(parts) == 4:
            parts.append("nan")  # recorded before the fx column existed
        if len(parts) != 5:
            continue
        name, g, r, s, x = parts
        rows.append((name, float(g), float(r), float(s), float(x)))
    out = [
        f"Best-of-N total run time at 200&times;50 with pacing disabled "
        f"(`--frame-rate 0`), seed 1, on `{machine}`. "
        "Speedups above 1.00&times; mean glyphfx is faster.",
        "",
        "| effect | glyphfx | ttfx (fx) | vs fx | ttfx (asm) | vs asm | ttfx (Rust) | vs Rust |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
    ]

    def col(v, g):
        if v == v and v > 0:  # not NaN
            return f"{v:.1f} ms", (f"**{v / g:.2f}&times;**" if g else "n/a")
        return "-", "-"

    for name, g, r, s, x in rows:
        fx, vs_x = col(x, g)
        asm, vs_s = col(s, g)
        vs_r = f"{r / g:.2f}&times;" if g else "n/a"
        out.append(f"| {name} | {g:.1f} ms | {fx} | {vs_x} | {asm} | {vs_s} | {r:.1f} ms | {vs_r} |")
    out.append("")
    out.append("Reproduce with `python3 tools/tests/matrix.py` "
               "(`GLYPHFX_TTFX_FX` names a ttfx >= 0.5 binary for the fx column, "
               "`GLYPHFX_TTFX_ASM` a build of ttfx 73c7a44 for the asm column), "
               "and `make perf` to gate against regression.")
    return "\n".join(out)


def effects_table(effects=EFFECTS):
    rows = []
    for i in range(0, len(effects), 2):
        cells = []
        for name, desc in effects[i:i + 2]:
            cells.append(f"| <b>{name}</b><br><img src=\"docs/effects/{name}.gif\" width=\"400\" alt=\"{name}\"><br><sub>{desc}</sub> ")
        while len(cells) < 2:
            cells.append("| ")
        rows.append("".join(cells) + "|")
    return "\n".join(rows)


README = f"""# glyphfx

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
removed again in [7444eea](https://github.com/omacom/ttfx/commit/7444eea), once
ttfx's portable Rust engine (`fx`) matched or beat it on every effect. glyphfx
draws the same conclusion:

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
{effects_table()}

## glyphfx originals

Four effects that ttfx does not have, from the SolverForge demoscene
productions. They follow the same contract (text in, the text in place at the
end; `burnout` is the one exit effect) but sit outside byte parity:
`--random-effect` draws them only when they are named in `--include-effects`,
and `make originals` holds them to determinism, golden streams and a
final-frame check instead. Details in [docs/ORIGINALS.md](docs/ORIGINALS.md).

|     |     |
|:---:|:---:|
{effects_table(ORIGINALS)}

## Benchmarks

Startup is under a millisecond (about 0.3 ms for `glyphfx --version`). The full
37-effect matrix against ttfx's engines: `fx`, the fastest port and glyphfx's
target; the removed assembly engine, for the record; and the Rust 0.3.3 engine
that is glyphfx's byte-parity oracle:

{matrix_table()}

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
"""


def main():
    open(os.path.join(ROOT, "README.md"), "w").write(README)
    print("wrote README.md")


if __name__ == "__main__":
    main()
