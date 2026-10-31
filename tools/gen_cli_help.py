#!/usr/bin/env python3
"""Generate src/cli_help.c: the text `glyphfx --help` prints for every option.

The 37 ttfx effects take their one-line descriptions, option help and
defaults from the reference ttfx's own --help (the oracle glyphfx reproduces),
so the two CLIs describe the same option the same way. The four glyphfx
originals have no ttfx counterpart; their text comes from the option tables
in docs/ORIGINALS.md. ttfx leaves most root options undescribed, so those
descriptions are written here, against the engine's behaviour.

The output is committed, like README.md: building glyphfx never needs ttfx.
Re-run after an option changes:

    tools/gen_cli_help.py            # writes src/cli_help.c
    tools/gen_cli_help.py --check    # exit 1 if src/cli_help.c is stale
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "src/cli_help.c"
TTFX = os.environ.get("GLYPHFX_TTFX", str(ROOT / "reference/target/release/ttfx"))

# Root options in ROOT_OPTS order (hidden test options excluded):
# (name, short, value placeholder, help, default).
ROOT_OPTIONS = [
    ("input-file", "i", "FILE", "File to read input from; without it glyphfx reads stdin", None),
    ("tab-width", None, "N", "Number of spaces a tab character expands to", "4"),
    ("xterm-colors", None, None, "Convert RGB colors to the closest xterm-256 color", None),
    ("no-color", None, None, "Disable all colors in the effect", None),
    ("terminal-background-color", None, "COLOR",
     "Background color of the terminal, used to fade characters in and out", "000000"),
    ("existing-color-handling", None, "MODE",
     "How ANSI colors already in the input are treated: always, dynamic or ignore", "ignore"),
    ("wrap-text", None, None, "Wrap lines wider than the canvas", None),
    ("frame-rate", None, "FPS", "Target frames per second; 0 runs as fast as possible", "60"),
    ("canvas-width", None, "COLS",
     "Canvas width: -1 fits the input, 0 fills the terminal, n is exact", "-1"),
    ("canvas-height", None, "ROWS",
     "Canvas height: -1 fits the input, 0 fills the terminal, n is exact", "-1"),
    ("anchor-canvas", None, "ANCHOR",
     "Where the canvas sits in the terminal: n ne e se s sw w nw c", "sw"),
    ("anchor-text", None, "ANCHOR", "Where the text sits in the canvas: n ne e se s sw w nw c", "sw"),
    ("ignore-terminal-dimensions", None, None,
     "Size the canvas from the input even when it exceeds the terminal", None),
    ("reuse-canvas", None, None, "Draw over the previous output instead of below it", None),
    ("no-eol", None, None, "Do not print a newline after the final frame", None),
    ("no-restore-cursor", None, None, "Leave the cursor hidden after the effect", None),
    ("seed", None, "SEED", "Seed for the random number generator; the same seed replays the same run", None),
    ("print-completion", None, "SHELL", "Print a shell completion script and exit: bash or zsh", None),
    ("random-effect", "R", None, "Run a random effect", None),
    ("include-effects", None, "EFFECT", "Limit --random-effect to these effects", None),
    ("exclude-effects", None, "EFFECT", "Exclude these effects from --random-effect", None),
    ("version", "v", None, "Print the version and exit", None),
    ("help", "h", None, "Print this help; with an effect, that effect's options", None),
]
MULTI_ROOT = {"include-effects", "exclude-effects"}

TTFX_EFFECTS = [
    "beams", "binarypath", "blackhole", "bouncyballs", "bubbles", "burn", "colorshift",
    "crumble", "decrypt", "errorcorrect", "expand", "fireworks", "highlight", "laseretch",
    "matrix", "middleout", "orbittingvolley", "overflow", "pour", "print", "rain",
    "randomsequence", "rings", "scattered", "slice", "slide", "smoke", "spotlights", "spray",
    "swarm", "sweep", "synthgrid", "thunderstorm", "unstable", "vhstape", "waves", "wipe",
]

# One-line descriptions of the originals (as in README.md and docs/ORIGINALS.md).
ORIGINALS = [
    ("bannerwave", "A bright front reveals the text while every column rides a travelling sine, "
                   "then the text lies flat"),
    ("burnout", "Exit effect: the text holds, then burns away through ember colours, shades and rising sparks"),
    ("comet", "Characters fly arcs from the bottom of the canvas on fading trails, land bright and cool into place"),
    ("copper", "Copper bars bounce behind dimmed text, lighting whole rows, then a sweep settles the final colours"),
]
# Option kinds that take several values, for the "..." suffix.
ORIGINALS_MULTI = {"trail-symbols", "bar-colors", "burn-symbols", "ember-colors",
                   "final-gradient-stops", "final-gradient-steps"}

OPT_RE = re.compile(r"^\s+(?:-(\w), )?--([a-z0-9-]+)(?: <([A-Z0-9_]+)>)?(\.\.\.)?\s*$")
DEFAULT_RE = re.compile(r"\s*\[default: ([^\]]*)\]\s*$")


def ttfx_help(args: list[str]) -> str:
    p = subprocess.run([TTFX, *args, "--help"], capture_output=True, text=True, timeout=30)
    if p.returncode != 0:
        sys.exit(f"{TTFX} {' '.join(args)} --help exited {p.returncode}")
    return p.stdout


def parse_options(text: str) -> list[tuple]:
    """(name, short, value, multi, help, default) for each option but --help."""
    lines = text.split("\n")
    start = lines.index("Options:") + 1
    opts, cur = [], None
    for line in lines[start:]:
        m = OPT_RE.match(line)
        if m:
            cur = [m.group(2), m.group(1), m.group(3), bool(m.group(4)), []]
            opts.append(cur)
        elif cur is not None and line.strip():
            cur[4].append(line.strip())
    out = []
    for name, short, value, multi, body in opts:
        if name == "help":
            continue
        text = " ".join(body)
        default = None
        d = DEFAULT_RE.search(text)
        if d:
            default = d.group(1)
            text = text[: d.start()].rstrip()
        out.append((name, short, value, multi, text, default))
    return out


def originals_options() -> dict[str, list[tuple]]:
    """Option tables of docs/ORIGINALS.md, keyed by effect, plus the shared gradient options."""
    md = (ROOT / "docs/ORIGINALS.md").read_text()
    tables: dict[str, list[tuple]] = {}
    effect = None
    for line in md.split("\n"):
        h = re.match(r"^\*\*([a-z]+)\*\*\s*$", line)
        if h:
            effect = h.group(1)
            continue
        row = re.match(r"^\| `--([a-z-]+)` \| `([^`]*)` \| (.*?) \|$", line)
        if row and effect:
            name, default, desc = row.groups()
            desc = desc[0].upper() + desc[1:]
            tables.setdefault(effect, []).append(
                (name, None, name.upper().replace("-", "_"), name in ORIGINALS_MULTI, desc, default))
    shared = re.search(r"Every effect also takes `--final-gradient-stops` \(default\s+`([^`]*)`\), "
                       r"`--final-gradient-steps` \(`([^`]*)`\) and\s+`--final-gradient-direction` "
                       r"\(`([^`]*)`\)", md)
    if not shared:
        sys.exit("docs/ORIGINALS.md: shared final-gradient sentence not found")
    stops, steps, direction = shared.groups()
    for effect in tables:
        tables[effect] += [
            ("final-gradient-stops", None, "FINAL_GRADIENT_STOPS", True,
             "Space separated, unquoted, list of colors for the final gradient", stops),
            ("final-gradient-steps", None, "FINAL_GRADIENT_STEPS", True,
             "Number of gradient steps to use", steps),
            ("final-gradient-direction", None, "FINAL_GRADIENT_DIRECTION", False,
             "Direction of the final gradient", direction),
        ]
    return tables


def c_str(s: str | None) -> str:
    if s is None:
        return "NULL"
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def opt_row(name, short, value, multi, text, default) -> str:
    sc = f"'{short}'" if short else "0"
    return (f"    {{{c_str(name)}, {sc}, {c_str(value)}, {'true' if multi else 'false'}, "
            f"{c_str(text)}, {c_str(default)}}},")


def render() -> str:
    effects = []
    for e in TTFX_EFFECTS:
        text = ttfx_help([e])
        about = text.split("\n", 1)[0].strip()
        effects.append((e, about, parse_options(text)))
    tables = originals_options()
    for e, about in ORIGINALS:
        if e not in tables:
            sys.exit(f"docs/ORIGINALS.md: no option table for {e}")
        effects.append((e, about, tables[e]))

    out = ["// Generated by tools/gen_cli_help.py from the reference ttfx --help and",
           "// docs/ORIGINALS.md. Do not edit by hand: re-run the generator.",
           '#include "cli_help.h"', "", "#include <string.h>", "",
           "const CliOptHelp CLI_ROOT_HELP[] = {"]
    out += [opt_row(n, s, v, n in MULTI_ROOT, h, d) for n, s, v, h, d in ROOT_OPTIONS]
    out += ["};", "const size_t CLI_ROOT_HELP_COUNT = sizeof(CLI_ROOT_HELP) / sizeof(CLI_ROOT_HELP[0]);", ""]
    for e, _, opts in effects:
        out.append(f"static const CliOptHelp OPTS_{e}[] = {{")
        out += [opt_row(*o) for o in opts]
        out += ["};", ""]
    out.append("const CliEffectHelp CLI_EFFECT_HELP[] = {")
    for e, about, _ in effects:
        out.append(f"    {{{c_str(e)}, {c_str(about)}, OPTS_{e}, sizeof(OPTS_{e}) / sizeof(OPTS_{e}[0])}},")
    out += ["};", "const size_t CLI_EFFECT_HELP_COUNT = sizeof(CLI_EFFECT_HELP) / sizeof(CLI_EFFECT_HELP[0]);", "",
            "const CliEffectHelp *cli_help_effect(const char *name) {",
            "    for (size_t i = 0; i < CLI_EFFECT_HELP_COUNT; i++) {",
            "        if (strcmp(CLI_EFFECT_HELP[i].effect, name) == 0) {",
            "            return &CLI_EFFECT_HELP[i];",
            "        }",
            "    }",
            "    return NULL;",
            "}", "",
            "const CliOptHelp *cli_help_option(const CliOptHelp *opts, size_t n, const char *name) {",
            "    for (size_t i = 0; i < n; i++) {",
            "        if (strcmp(opts[i].name, name) == 0) {",
            "            return &opts[i];",
            "        }",
            "    }",
            "    return NULL;",
            "}", ""]
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="fail if src/cli_help.c is stale")
    a = ap.parse_args()
    text = render()
    if a.check:
        if not OUT.exists() or OUT.read_text() != text:
            print(f"{OUT.relative_to(ROOT)} is stale: run tools/gen_cli_help.py")
            return 1
        print(f"{OUT.relative_to(ROOT)} is current")
        return 0
    OUT.write_text(text)
    print(f"wrote {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
