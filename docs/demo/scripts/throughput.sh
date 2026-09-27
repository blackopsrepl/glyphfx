#!/usr/bin/env bash
# Throughput demo: ms per rendered frame with pacing disabled (--frame-rate 0),
# glyphfx vs ttfx, best of 3. Compact so the clip stays short.
set -u
cd /srv/lab/hack/glyphfx
exec python3 - <<'PY'
import os, subprocess, time

ROOT = "/srv/lab/hack/glyphfx"
GF = ROOT + "/build/glyphfx"
TTFX = ROOT + "/reference/target/release/ttfx"
COLS, ROWS, FRAMES, RUNS = 140, 36, 150, 3
CASES = [("slide", []), ("beams", []), ("blackhole", []), ("matrix", ["--rain-time", "1"])]

banner = subprocess.run(["figlet", "-f", "standard", "GLYPHFX"], stdout=subprocess.PIPE).stdout

def run(binary, effect, extra):
    cmd = [binary, "--seed", "1", "--parity-dump", "--max-frames", str(FRAMES), "--virtual-clock",
           "--canvas-width", str(COLS), "--canvas-height", str(ROWS), effect] + extra
    env = dict(os.environ); env["COLUMNS"] = str(COLS); env["LINES"] = str(ROWS)
    t = time.perf_counter()
    p = subprocess.run(cmd, input=banner, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env)
    el = time.perf_counter() - t
    frames = int(next((l.split("=")[1] for l in p.stderr.decode().splitlines() if l.startswith("frames=")), "0"))
    return el, frames

green = "\033[1;38;2;16;185;129m"; orange = "\033[1;38;2;255;130;80m"
bold = "\033[1m"; dim = "\033[2m"; reset = "\033[0m"

print(f"\n\n  {bold}throughput{reset}  ·  ms per rendered frame, pacing disabled {dim}(--frame-rate 0){reset}")
print(f"  {dim}canvas {COLS}x{ROWS}, {FRAMES} frames, best of {RUNS}{reset}\n")
print(f"  {bold}{'effect':<12}{'glyphfx':>12}{'ttfx':>12}{'speedup':>12}{reset}")
for effect, extra in CASES:
    bg = (1e9, 0); bt = (1e9, 0)
    for _ in range(RUNS):
        e, f = run(GF, effect, extra);   bg = min(bg, (e, f))
        e, f = run(TTFX, effect, extra); bt = min(bt, (e, f))
    gpf = bg[0] / max(bg[1], 1) * 1000.0
    tpf = bt[0] / max(bt[1], 1) * 1000.0
    print(f"  {effect:<12}{green}{gpf:>10.3f} ms{reset}{orange}{tpf:>10.3f} ms{reset}"
          f"{green}{tpf / gpf:>11.2f}x{reset}")
print(f"\n  {dim}speedup >1 means glyphfx is faster; same canvas, frames, and seed{reset}\n")
PY
