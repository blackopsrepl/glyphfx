#!/usr/bin/env python3
"""Full 37-effect performance matrix: glyphfx vs the ttfx asm engine.

The asm build is the performance target; the Rust 0.3.3 port is recorded
alongside as provenance (parity with it is already established) and doubles as
the byte-parity oracle.

Fixed workload: 200x50 canvas, 190x46 text, seed 1, --frame-rate 0
--virtual-clock, pinned, best of N, output to /dev/null. Every run must exit 0.

Writes docs/benchmarks/matrix.tsv (machine header + one row per effect) and
prints a markdown table. gen_readme.py renders the README table from this file,
so the numbers are a recorded measurement, not hand-typed.

Usage: tools/tests/matrix.py [--runs N]
"""
from __future__ import annotations

import argparse
import os
import platform
import random
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "docs/benchmarks/matrix.tsv"
GF = str(ROOT / "build/glyphfx")
RUST = os.environ.get("GLYPHFX_TTFX", str(ROOT / "reference/target/release/ttfx"))
ASM = os.environ.get("GLYPHFX_TTFX_ASM", "/tmp/opencode/ttfx/target/release/ttfx")
EFFECTS = ["beams", "binarypath", "blackhole", "bouncyballs", "bubbles", "burn",
    "colorshift", "crumble", "decrypt", "errorcorrect", "expand", "fireworks",
    "highlight", "laseretch", "matrix", "middleout", "orbittingvolley", "overflow",
    "pour", "print", "rain", "randomsequence", "rings", "scattered", "slice", "slide",
    "smoke", "spotlights", "spray", "swarm", "sweep", "synthgrid", "thunderstorm",
    "unstable", "vhstape", "waves", "wipe"]
COLS, ROWS, SEED, TIMEOUT = 200, 50, 1, 300


def workload_input() -> bytes:
    rng = random.Random(1)
    words = ("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi "
             "omicron pi rho sigma tau upsilon phi chi psi omega solver forge constraint "
             "schedule route").split()
    lines = []
    for _ in range(46):
        s = ""
        while len(s) < 190:
            s += rng.choice(words) + " "
        lines.append(s[:190])
    return ("\n".join(lines) + "\n").encode()


def machine_key() -> str:
    cpu = ""
    if Path("/proc/cpuinfo").exists():
        m = re.search(r"^model name\s*:\s*(.+)$", Path("/proc/cpuinfo").read_text(), re.M)
        if m:
            cpu = m.group(1).strip()
    if not cpu and platform.system() == "Darwin":
        cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                             capture_output=True, text=True).stdout.strip()
    return f"{platform.machine()}:{cpu or platform.processor() or 'unknown'}"


def pin() -> list[str]:
    if platform.system() == "Linux":
        from shutil import which
        if which("taskset"):
            try:
                allowed = sorted(os.sched_getaffinity(0))
            except AttributeError:
                allowed = []
            if allowed:
                return ["taskset", "-c", str(allowed[0])]
    return []


def measure(binary: str, effect: str, data: bytes, runs: int, extra_env: dict) -> float:
    cmd = pin() + [binary, "--seed", str(SEED), "--frame-rate", "0", "--virtual-clock",
                   "--canvas-width", str(COLS), "--canvas-height", str(ROWS), effect]
    env = dict(os.environ); env.update(extra_env); env["COLUMNS"] = str(COLS); env["LINES"] = str(ROWS)
    best = float("inf")
    for _ in range(runs):
        t = time.perf_counter()
        p = subprocess.run(cmd, input=data, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                           env=env, timeout=TIMEOUT)
        if p.returncode != 0:
            raise SystemExit(f"{Path(binary).name} {effect}: exit {p.returncode}")
        best = min(best, time.perf_counter() - t)
    return best * 1000.0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=5)
    a = ap.parse_args()
    data = workload_input()
    have_asm = Path(ASM).exists()
    print(f"matrix: {len(EFFECTS)} effects, best of {a.runs}, machine {machine_key()}")
    print(f"  glyphfx {GF}\n  rust    {RUST}\n  asm     {ASM if have_asm else '(not built)'}")
    rows = []
    for e in EFFECTS:
        g = measure(GF, e, data, a.runs, {})
        r = measure(RUST, e, data, a.runs, {"TTFX_ASM": "0"})
        s = measure(ASM, e, data, a.runs, {}) if have_asm else float("nan")
        rows.append((e, g, r, s))
        extra = f"  vs asm {s:7.1f} ({g/s:4.2f}x)" if have_asm else ""
        print(f"  {e:<15} glyphfx {g:7.1f}{extra}  rust(ref) {r:7.1f}", flush=True)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("w") as f:
        f.write(f"# machine {machine_key()}\n# best-of-{a.runs} ms, 200x50, seed 1\n")
        f.write("# effect\tglyphfx\trust\tasm\n")
        for e, g, r, s in rows:
            f.write(f"{e}\t{g:.1f}\t{r:.1f}\t{s:.1f}\n")
    print(f"wrote {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
