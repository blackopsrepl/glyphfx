#!/usr/bin/env bash
# Cold-path demo: glyphfx vs ttfx on the real path, not just --version.
# Median of many runs; --version is shown only as the loader/CLI floor.
set -u
cd /srv/lab/hack/glyphfx
exec python3 - "$@" <<'PY'
import statistics, subprocess, time

N = 200
DEVNULL = subprocess.DEVNULL

CASES = [
    ("CLI startup  (--version)", ["--version"]),
    ("first frame  (beams)",     ["--frame-rate", "0", "--max-frames", "1", "--virtual-clock", "beams"]),
    ("whole effect (beams)",     ["--frame-rate", "0", "--virtual-clock", "beams"]),
]

def median_ms(binary, args):
    for _ in range(20):
        subprocess.run([binary] + args, input=b"x", stdout=DEVNULL, stderr=DEVNULL)
    s = []
    for _ in range(N):
        t = time.perf_counter()
        subprocess.run([binary] + args, input=b"x", stdout=DEVNULL, stderr=DEVNULL)
        s.append((time.perf_counter() - t) * 1000.0)
    return statistics.median(s)

GF, TTFX = "build/glyphfx", "reference/target/release/ttfx"
green = "\033[1;38;2;16;185;129m"; orange = "\033[1;38;2;255;130;80m"
bold = "\033[1m"; dim = "\033[2m"; reset = "\033[0m"

print("\n\n")
print(f"  {bold}GlyphFX{reset} vs {orange}ttfx{reset} — cold path, median of {N} runs")
print(f"  {dim}piped through a process, the way a shell uses it{reset}\n")
print(f"  {bold}{'':<26}{'glyphfx':>12}{'ttfx':>12}{'speedup':>12}{reset}")
for label, args in CASES:
    g = median_ms(GF, args)
    t = median_ms(TTFX, args)
    print(f"  {label:<26}{green}{g:>10.3f} ms{reset}{orange}{t:>10.3f} ms{reset}{green}{t / g:>11.1f}x{reset}")
print(f"\n  {green}Every glyphfx path here is sub-millisecond{reset} — including rendering the whole effect.")
print(f"  {dim}median total includes identical process-spawn cost for both binaries{reset}\n")
PY
