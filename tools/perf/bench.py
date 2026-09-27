#!/usr/bin/env python3
"""Per-effect CPU time and instruction counts for the C engine against ttfx.

The acceptance metric is emoon's tier-3 asm engine, measured the way his own
harness measures it (tools/asm/speed.py): the oracle's 190x46 text, 200x50
canvas, --frame-rate 0, seed 1, pinned to one core, best of N runs, child
user+system CPU time. Instructions per effect are reported alongside, since
they are load-independent and were stable to ~0.1% within a binary.

Usage:
  tools/perf/bench.py                      # every effect, 3 runs, 4 effects' summary
  tools/perf/bench.py print errorcorrect   # named effects
  tools/perf/bench.py --runs 5 --json out.json
  tools/perf/bench.py --baseline           # write/refresh the baseline file
"""
import argparse
import json
import math
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OURS = os.environ.get("GLYPHFX_BIN", os.path.join(ROOT, "build/glyphfx"))
THEIRS = os.environ.get("TTFX_ASM_BIN", "/tmp/opencode/ttfx/target/release/ttfx")
BASELINE = os.path.join(ROOT, "tools/perf/bench_baseline.json")
INPUT = "/tmp/opencode/bench/input.txt"

ALL = ("beams binarypath blackhole bouncyballs bubbles burn colorshift crumble decrypt errorcorrect "
       "expand fireworks highlight laseretch matrix middleout orbittingvolley overflow pour print rain "
       "randomsequence rings scattered slice slide smoke spotlights spray swarm sweep synthgrid "
       "thunderstorm unstable vhstape waves wipe").split()


def make_input(path):
    if os.path.exists(path):
        return
    line = ("The quick brown fox jumps over the lazy dog 0123456789 " * 4)[:190]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write("\n".join(line for _ in range(46)))


def args_for(binary, effect):
    return ["taskset", "-c", CORE, binary, "--seed", "1", "--frame-rate", "0", "--virtual-clock",
            "--canvas-width", "200", "--canvas-height", "50", "--ignore-terminal-dimensions", effect]


def cpu_time(binary, effect, runs):
    best = math.inf
    for _ in range(runs):
        with open(INPUT) as stdin:
            p = subprocess.Popen(args_for(binary, effect), stdin=stdin, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.PIPE, env=ENV)
            p.stderr.read()
            _, status, usage = os.wait4(p.pid, 0)
        if os.waitstatus_to_exitcode(status) != 0:
            raise SystemExit(f"{effect}: {binary} exited nonzero")
        best = min(best, usage.ru_utime + usage.ru_stime)
    return best * 1000.0


def instructions(binary, effect):
    p = subprocess.run(["taskset", "-c", CORE, "perf", "stat", "-x", ";", "-e", "instructions", "--",
                        binary, "--seed", "1", "--frame-rate", "0", "--virtual-clock",
                        "--canvas-width", "200", "--canvas-height", "50", "--ignore-terminal-dimensions", effect],
                       stdin=open(INPUT), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=ENV)
    for line in p.stderr.decode().splitlines():
        if ";instructions;" in line:
            return int(line.split(";")[0])
    return 0


def main():
    global CORE, ENV
    ap = argparse.ArgumentParser()
    ap.add_argument("effects", nargs="*")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--core", default=os.environ.get("BENCH_CORE", "8"))
    ap.add_argument("--json", default="")
    ap.add_argument("--baseline", action="store_true")
    a = ap.parse_args()
    CORE = a.core
    ENV = dict(os.environ, COLUMNS="200", LINES="50")
    effects = a.effects or ALL
    make_input(INPUT)
    rows = {}
    print(f"{'effect':16}{'ours ms':>9}{'asm ms':>9}{'cpu x':>7}{'ours M':>9}{'asm M':>8}{'ins x':>7}")
    for e in effects:
        oc = cpu_time(OURS, e, a.runs)
        ac = cpu_time(THEIRS, e, a.runs)
        oi = instructions(OURS, e)
        ai = instructions(THEIRS, e)
        rows[e] = {"ours_ms": oc, "asm_ms": ac, "cpu_x": ac / oc, "ours_ins": oi, "asm_ins": ai,
                   "ins_x": ai / oi if oi else 0.0}
        print(f"{e:16}{oc:9.1f}{ac:9.1f}{ac/oc:7.2f}{oi/1e6:9.1f}{ai/1e6:8.1f}{ai/oi:7.2f}", flush=True)
    g_cpu = math.exp(sum(math.log(r["cpu_x"]) for r in rows.values()) / len(rows))
    g_ins = math.exp(sum(math.log(r["ins_x"]) for r in rows.values() if r["ins_x"]) / len(rows))
    print(f"{'geomean':16}{'':>9}{'':>9}{g_cpu:7.2f}{'':>9}{'':>8}{g_ins:7.2f}")
    if a.baseline:
        with open(BASELINE, "w") as f:
            json.dump({"target": THEIRS, "rows": rows, "geomean_cpu": g_cpu, "geomean_ins": g_ins}, f, indent=1)
        print(f"baseline written: {BASELINE}")
    if a.json:
        with open(a.json, "w") as f:
            json.dump(rows, f, indent=1)


if __name__ == "__main__":
    main()
