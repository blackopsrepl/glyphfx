#!/usr/bin/env python3
"""Measure glyphfx vs the ttfx binary: milliseconds per rendered frame with
pacing disabled (--frame-rate 0), best of a few runs."""
import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ORACLE = os.environ.get("GLYPHFX_TTFX", os.path.join(ROOT, "reference/target/release/ttfx"))
SUBJECT = os.environ.get("GLYPHFX_BIN", os.path.join(ROOT, "build/glyphfx"))
COLS = int(os.environ.get("GLYPHFX_BENCH_COLS", "160"))
ROWS = int(os.environ.get("GLYPHFX_BENCH_LINES", "40"))
FRAMES = int(os.environ.get("GLYPHFX_BENCH_FRAMES", "300"))
RUNS = 3

CASES = [
    ("slide", []),
    ("beams", []),
    ("waves", []),
    ("rings", []),
    ("blackhole", []),
    ("matrix", ["--rain-time", "1"]),
]


def banner():
    return subprocess.run(["figlet", "-f", "standard", "GLYPHFX"], stdout=subprocess.PIPE,
                          check=True).stdout


def run(binary, effect, extra):
    cmd = [binary, "--seed", "1", "--parity-dump", "--max-frames", str(FRAMES), "--virtual-clock",
           "--canvas-width", str(COLS), "--canvas-height", str(ROWS), effect] + extra
    env = dict(os.environ)
    env["COLUMNS"] = str(COLS)
    env["LINES"] = str(ROWS)
    start = time.perf_counter()
    proc = subprocess.run(cmd, input=banner(), stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
    elapsed = time.perf_counter() - start
    frames = 0
    for line in proc.stderr.decode().splitlines():
        if line.startswith("frames="):
            frames = int(line.split("=")[1])
    return elapsed, frames


def main():
    print(f"canvas {COLS}x{ROWS}, max-frames {FRAMES}, best of {RUNS}, ms/frame")
    print(f"{'effect':<12}{'frames':>8}{'glyphfx':>12}{'ttfx':>12}{'speedup':>10}")
    for effect, extra in CASES:
        best_g = (1e9, 0)
        best_t = (1e9, 0)
        for _ in range(RUNS):
            e, f = run(SUBJECT, effect, extra)
            if e < best_g[0]:
                best_g = (e, f)
            e, f = run(ORACLE, effect, extra)
            if e < best_t[0]:
                best_t = (e, f)
        gpf = best_g[0] / max(best_g[1], 1) * 1000
        tpf = best_t[0] / max(best_t[1], 1) * 1000
        print(f"{effect:<12}{max(best_g[1], best_t[1]):>8}{gpf:>12.3f}{tpf:>12.3f}{tpf / gpf:>9.1f}x")


if __name__ == "__main__":
    main()
