#!/usr/bin/env python3
"""Behaviour and golden suite for the glyphfx originals (comet, copper, bannerwave, burnout).

The originals have no ttfx counterpart, so `make effects` cannot vouch for them.
This suite holds them to the same standard by other means:

  determinism  every case run twice yields a byte-identical frame stream
  goldens      each stream's sha256 matches tests/fixtures/originals_goldens.tsv
               (pinned to Linux/glibc like the parity suite; --no-goldens skips)
  final frame  entrance effects end with exactly the text wipe leaves on the
               same canvas; burnout, the exit effect, ends with an empty canvas
  surface      colour modes, tiny and oversized inputs, and invalid option
               values exit with the codes the CLI contract promises

Usage: tools/tests/originals.py [--update] [--no-goldens]
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = os.environ.get("GLYPHFX_BIN", str(ROOT / "build/glyphfx"))
GOLDENS = ROOT / "tests/fixtures/originals_goldens.tsv"
ENTRANCE = ["bannerwave", "comet", "copper"]
EXIT = ["burnout"]
CSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]")

BANNER = (ROOT / "tools/demo/banner.txt").read_bytes()
PROSE = (b"Constraint solvers schedule the world quietly.\n"
         b"  Every shift, route and timetable is a search.\n"
         b"glyphfx draws the result in the terminal.\n")
ANSI = b"\x1b[38;5;196mred\x1b[0m plain \x1b[48;2;0;80;160mblue bg\x1b[0m\n\x1b[1;32mbold green\x1b[0m\n"
INPUTS = {"banner": BANNER, "prose": PROSE, "ansi": ANSI, "one": b"x\n"}

# (effect, input, canvas WxH, seed, extra terminal args, effect args)
CASES = []
for e in ENTRANCE + EXIT:
    CASES += [
        (e, "banner", (63, 14), 1, [], []),
        (e, "banner", (63, 14), 7, [], []),
        (e, "prose", (80, 10), 3, [], []),
        (e, "one", (20, 5), 1, [], []),
        (e, "ansi", (40, 6), 2, ["--existing-color-handling", "dynamic"], []),
        (e, "ansi", (40, 6), 2, ["--existing-color-handling", "always"], []),
        (e, "prose", (80, 10), 3, ["--xterm-colors"], []),
        (e, "prose", (80, 10), 3, ["--no-color"], []),
        (e, "banner", (30, 4), 1, [], []),  # text larger than the canvas
    ]
CASES += [
    ("comet", "banner", (63, 14), 1, [], ["--launch-order", "center_to_outside", "--trail-length", "0",
                                          "--arc-height", "1-2", "--flight-ease", "linear"]),
    ("comet", "prose", (80, 10), 5, [], ["--trail-symbols", "*", "+", "--comet-color", "ff00ff"]),
    ("copper", "banner", (63, 14), 1, [], ["--bar-colors", "ff0000", "00ff00", "0000ff", "ffff00",
                                           "--bar-height", "2", "--settle-frames", "1"]),
    ("bannerwave", "banner", (63, 14), 1, [], ["--wave-amplitude", "0", "--wave-frames", "0"]),
    ("bannerwave", "prose", (80, 10), 1, [], ["--wave-length", "8", "--wave-period", "20"]),
    ("burnout", "banner", (63, 14), 1, [], ["--burn-direction", "row_top_to_bottom", "--spark-ratio", "1",
                                            "--burn-jitter", "0", "--hold-frames", "0"]),
    ("burnout", "prose", (80, 10), 9, [], ["--spark-ratio", "0", "--ember-colors", "ffffff",
                                           "--burn-symbols", "#"]),
]

# (args, expected exit code): the CLI's usage-error contract.
BAD = [
    (["comet", "--flight-frames", "0"], 2),
    (["comet", "--arc-height", "5-2"], 2),
    (["comet", "--trail-symbols", "ab"], 2),
    (["copper", "--bar-colors", "nothex"], 2),
    (["bannerwave", "--wave-amplitude", "-1"], 2),
    (["burnout", "--spark-ratio", "1.5"], 2),
    (["burnout", "--burn-direction", "sideways"], 2),
]


def run(effect, data, canvas, seed, term, eff):
    cols, rows = canvas
    env = dict(os.environ, COLUMNS=str(cols), LINES=str(rows))
    cmd = [BIN, "--seed", str(seed), "--parity-dump", "--virtual-clock", "--frame-rate", "0",
           "--canvas-width", str(cols), "--canvas-height", str(rows), *term, effect, *eff]
    p = subprocess.run(cmd, input=data, capture_output=True, env=env, timeout=300)
    return p.returncode, p.stdout, p.stderr


def frames(stream):
    out, i = [], 0
    while i < len(stream):
        nl = stream.index(b"\n", i)
        n = int(stream[i:nl])
        out.append(stream[nl + 1:nl + 1 + n])
        i = nl + 1 + n + 1
    return out


def text_of(frame):
    lines = CSI.sub(b"", frame).decode("utf-8", "replace").split("\n")
    return "\n".join(line.rstrip() for line in lines).strip("\n")


def case_id(c):
    effect, inp, (w, h), seed, term, eff = c
    return " ".join([effect, inp, f"{w}x{h}", f"seed={seed}", *term, *eff])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true", help="rewrite the goldens from this build")
    ap.add_argument("--no-goldens", action="store_true", help="skip the glibc-pinned sha256 check")
    a = ap.parse_args()
    if not Path(BIN).exists():
        print(f"originals: binary not found: {BIN} (run make)", file=sys.stderr)
        return 2

    goldens = {}
    if GOLDENS.exists():
        for line in GOLDENS.read_text().splitlines():
            if line and not line.startswith("#"):
                key, frames_n, sha = line.split("\t")
                goldens[key] = (int(frames_n), sha)

    fails, recorded = [], {}
    for c in CASES:
        effect, inp, canvas, seed, term, eff = c
        key = case_id(c)
        rc1, s1, err = run(effect, INPUTS[inp], canvas, seed, term, eff)
        rc2, s2, _ = run(effect, INPUTS[inp], canvas, seed, term, eff)
        if rc1 != 0 or rc2 != 0:
            fails.append(f"{key}: exit {rc1}/{rc2}: {err.decode(errors='replace').strip()}")
            continue
        if s1 != s2:
            fails.append(f"{key}: two runs differ (nondeterministic)")
            continue
        fr = frames(s1)
        if not fr:
            fails.append(f"{key}: no frames")
            continue
        last = text_of(fr[-1])
        if effect in EXIT:
            if last.strip():
                fails.append(f"{key}: final frame is not empty")
        else:
            _, ws, _ = run("wipe", INPUTS[inp], canvas, seed, term, [])
            want = text_of(frames(ws)[-1])
            if last != want:
                fails.append(f"{key}: final frame text differs from the input layout")
        sha = hashlib.sha256(s1).hexdigest()
        recorded[key] = (len(fr), sha)
        if not a.update and not a.no_goldens:
            g = goldens.get(key)
            if g is None:
                fails.append(f"{key}: no golden (run tools/tests/originals.py --update)")
            elif g != (len(fr), sha):
                fails.append(f"{key}: golden mismatch ({len(fr)} frames {sha[:12]} vs {g[0]} {g[1][:12]})")

    for args, want in BAD:
        p = subprocess.run([BIN, *args], input=b"x\n", capture_output=True, timeout=30)
        if p.returncode != want or not p.stderr or p.stdout:
            fails.append(f"bad args {' '.join(args)}: rc {p.returncode} (want {want}, message on stderr)")
    for e in ENTRANCE + EXIT:
        p = subprocess.run([BIN, e, "--help"], capture_output=True, timeout=30)
        if p.returncode != 0 or b"Usage: glyphfx " + e.encode() not in p.stdout:
            fails.append(f"{e} --help: rc {p.returncode}")

    if a.update and not fails:
        lines = ["# glyphfx originals: frames and sha256 of the --parity-dump stream per case",
                 "# written by tools/tests/originals.py --update; Linux/glibc only"]
        lines += [f"{k}\t{n}\t{s}" for k, (n, s) in recorded.items()]
        GOLDENS.parent.mkdir(parents=True, exist_ok=True)
        GOLDENS.write_text("\n".join(lines) + "\n")
        print(f"originals: wrote {len(recorded)} goldens to {GOLDENS.relative_to(ROOT)}")

    total = len(CASES) + len(BAD) + len(ENTRANCE + EXIT)
    for f in fails:
        print(f"FAIL {f}")
    print(f"originals: {total - len(fails)}/{total} checks passed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
