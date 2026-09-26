#!/usr/bin/env python3
"""Performance regression gate (fail-closed).

For every effect this checks, in order:
  1. Correctness: a full `--parity-dump` run must exit 0, produce at least one
     frame, and hash to the same byte stream recorded in the baseline.
  2. Speed: the fixed workload timed best-of-N must not exceed the baseline.

Nothing that cannot be verified is allowed to pass:
  * a timed run that exits non-zero, times out, or is killed fails the gate;
  * a missing or malformed baseline fails the gate;
  * a baseline recorded on a different CPU fails the gate unless the caller
    explicitly passes --allow-machine;
  * --update refuses to record a run that regressed or changed output, unless
    --force is given, and it only ever lowers a time (tightens the bar).

The gate has no tolerance: any effect whose measured time exceeds its baseline
fails. The baseline is only ever lowered, so the bar rises with every verified
improvement. Re-baseline deliberately with --update after a verified change;
never widen the threshold to pass.

Baseline: tools/perf/baseline.tsv, machine-keyed, one row per effect:
    <effect>\t<best_of_ms>\t<frames>\t<sha256-of-parity-stream>

Usage:
  tools/perf/gate.py [--runs N] [--binary PATH] [--allow-machine]
  tools/perf/gate.py --update [--force]
"""
from __future__ import annotations

import argparse
import hashlib
import os
import platform
import random
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE = ROOT / "tools/perf/baseline.tsv"
SCHEMA = 1
EFFECTS = ["beams", "binarypath", "blackhole", "bouncyballs", "bubbles", "burn",
    "colorshift", "crumble", "decrypt", "errorcorrect", "expand", "fireworks",
    "highlight", "laseretch", "matrix", "middleout", "orbittingvolley", "overflow",
    "pour", "print", "rain", "randomsequence", "rings", "scattered", "slice", "slide",
    "smoke", "spotlights", "spray", "swarm", "sweep", "synthgrid", "thunderstorm",
    "unstable", "vhstape", "waves", "wipe"]

COLS, ROWS, SEED = 200, 50, 1
TIMEOUT = 300


class GateError(Exception):
    pass


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
    arch = platform.machine()
    cpu = ""
    if Path("/proc/cpuinfo").exists():
        m = re.search(r"^model name\s*:\s*(.+)$", Path("/proc/cpuinfo").read_text(), re.M)
        if m:
            cpu = m.group(1).strip()
    if not cpu and platform.system() == "Darwin":
        try:
            cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                                 capture_output=True, text=True, timeout=10).stdout.strip()
        except OSError:
            cpu = ""
    return f"{arch}:{cpu or platform.processor() or 'unknown'}"


def _which(name: str) -> str | None:
    from shutil import which
    return which(name)


def pin_prefix() -> list[str]:
    if platform.system() != "Linux" or not _which("taskset"):
        return []
    try:
        allowed = sorted(os.sched_getaffinity(0))
    except AttributeError:
        allowed = list(range(os.cpu_count() or 0))
    if not allowed:
        raise GateError("no CPU is available to pin a timed run to")
    return ["taskset", "-c", str(allowed[0])]


def _run(cmd: list[str], data: bytes, env: dict, capture_stdout: bool):
    out = subprocess.PIPE if capture_stdout else subprocess.DEVNULL
    try:
        return subprocess.run(cmd, input=data, stdout=out, stderr=subprocess.PIPE,
                              env=env, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        raise GateError(f"timed out after {TIMEOUT}s: {' '.join(cmd)}")


def _base_args(binary: str, effect: str) -> list[str]:
    return pin_prefix() + [binary, "--seed", str(SEED), "--frame-rate", "0", "--virtual-clock",
                           "--canvas-width", str(COLS), "--canvas-height", str(ROWS), effect]


def validate(binary: str, effect: str, data: bytes, env: dict) -> tuple[int, str]:
    """One full parity-dump run; returns (frames, sha256)."""
    cmd = pin_prefix() + [binary, "--seed", str(SEED), "--parity-dump", "--virtual-clock",
                          "--frame-rate", "0", "--canvas-width", str(COLS),
                          "--canvas-height", str(ROWS), effect]
    p = _run(cmd, data, env, capture_stdout=True)
    if p.returncode != 0:
        raise GateError(f"{effect}: validation run exited {p.returncode}")
    m = re.search(rb"^frames=(\d+)$", p.stderr, re.M)
    if not m or int(m.group(1)) < 1:
        raise GateError(f"{effect}: validation run produced no frames")
    return int(m.group(1)), hashlib.sha256(p.stdout).hexdigest()


def time_run(binary: str, effect: str, data: bytes, env: dict, runs: int) -> float:
    best = float("inf")
    for _ in range(runs):
        t = time.perf_counter()
        p = _run(_base_args(binary, effect), data, env, capture_stdout=False)
        dt = time.perf_counter() - t
        if p.returncode != 0:
            raise GateError(f"{effect}: timed run exited {p.returncode}")
        best = min(best, dt)
    return best * 1000.0


def load_baseline() -> tuple[str, dict[str, tuple[float, int, str]]]:
    key, vals = "", {}
    if BASELINE.exists():
        for line in BASELINE.read_text().splitlines():
            if line.startswith("# machine "):
                key = line[len("# machine "):].strip()
            elif line and not line.startswith("#"):
                parts = line.split("\t")
                if len(parts) != 4:
                    raise GateError(f"malformed baseline row: {line!r}")
                name, ms, frames, sha = parts
                vals[name] = (float(ms), int(frames), sha)
    return key, vals


def save_baseline(key: str, vals: dict[str, tuple[float, int, str]]) -> None:
    BASELINE.parent.mkdir(parents=True, exist_ok=True)
    lines = [f"# machine {key}", f"# schema {SCHEMA}",
             "# effect\tbest_of_ms\tframes\tsha256",
             "# times only ever tighten; frames and sha prove the workload still renders"]
    for e in EFFECTS:
        if e in vals:
            ms, frames, sha = vals[e]
            lines.append(f"{e}\t{ms:.3f}\t{frames}\t{sha}")
    BASELINE.write_text("\n".join(lines) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=str(ROOT / "build/glyphfx"))
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--allow-machine", action="store_true",
                    help="do not fail when the baseline was recorded on another CPU")
    ap.add_argument("--update", action="store_true", help="tighten the baseline to this run")
    ap.add_argument("--force", action="store_true", help="allow --update to record regressions")
    a = ap.parse_args()

    if not Path(a.binary).exists():
        print(f"perf gate: binary not found: {a.binary}", file=sys.stderr)
        return 2

    data = workload_input()
    env = dict(os.environ); env["COLUMNS"] = str(COLS); env["LINES"] = str(ROWS)
    key = machine_key()
    try:
        bkey, bvals = load_baseline()
    except GateError as exc:
        if not a.update:
            print(f"perf gate: {exc}", file=sys.stderr)
            return 2
        print(f"perf gate: ignoring unreadable baseline ({exc}); rebuilding", file=sys.stderr)
        bkey, bvals = "", {}

    if not bvals and not a.update:
        print("perf gate: no baseline; run tools/perf/gate.py --update once", file=sys.stderr)
        return 2
    if bvals and bkey != key and not a.allow_machine:
        print(f"perf gate: baseline is for '{bkey}' but this machine is '{key}'",
              file=sys.stderr)
        print("  fail-closed: re-baseline on this machine, or pass --allow-machine", file=sys.stderr)
        return 3

    print(f"perf gate: {len(EFFECTS)} effects, best of {a.runs}, machine {key}")
    measured: dict[str, tuple[float, int, str]] = {}
    regressions: list[tuple[str, float, float]] = []
    try:
        for e in EFFECTS:
            frames, sha = validate(a.binary, e, data, env)
            base = bvals.get(e)
            if base is not None:
                if frames != base[1] or sha != base[2]:
                    raise GateError(f"{e}: parity stream changed ({frames} frames, {sha[:12]})")
            ms = time_run(a.binary, e, data, env, a.runs)
            measured[e] = (ms, frames, sha)
            if base is not None and ms > base[0]:
                regressions.append((e, base[0], ms))
                mark = f"REGRESSED (baseline {base[0]:.1f})"
            else:
                mark = "new" if base is None else "ok"
            print(f"  {e:<15}{ms:>9.1f} ms  {mark}", flush=True)
    except GateError as exc:
        print(f"\nperf gate FAILED: {exc}", file=sys.stderr)
        return 1

    if a.update:
        if regressions and not a.force:
            print("\nperf gate: refusing to update over a regression "
                  "(use --force to override)", file=sys.stderr)
            return 1
        tightened = {}
        for e in EFFECTS:
            if e not in measured:
                continue
            old = bvals.get(e)
            tightened[e] = (min(old[0], measured[e][0]), measured[e][1], measured[e][2]) if old \
                else measured[e]
        save_baseline(key, tightened)
        improved = sum(1 for e in tightened if e in bvals and tightened[e][0] < bvals[e][0])
        print(f"baseline updated: {improved} effect(s) tightened, {BASELINE.relative_to(ROOT)}")
        return 0

    if regressions:
        print(f"\nperf gate FAILED: {len(regressions)} regression(s) vs baseline")
        for e, base, ms in regressions:
            print(f"  {e}: {base:.1f} -> {ms:.1f} ms (+{100 * (ms / base - 1):.0f}%)")
        return 1
    print("\nperf gate passed: no regression")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
