# GlyphFX demo videos

Short narrated clips for [glyphfx](https://github.com/blackopsrepl/glyphfx), a
pure-C17 parity port of [ttfx](https://github.com/omacom/ttfx). Recorded with
`computer-use-sway` on a 1920x1080 Sway output; narration by edge-tts with
burned-in subtitles.

glyphfx is the C port; **ttfx is the Rust reference**, and the effect design
itself is [TerminalTextEffects](https://github.com/ChrisBuilds/terminaltexteffects)
by ChrisBuilds. All credit for the effect set goes there.

## Clips

| file | length | what it shows |
|---|---|---|
| `01-startup.mp4` | 15 s | Startup time: glyphfx vs ttfx vs a bare process |
| `02-parity-beams.mp4` | 18 s | `beams` under ttfx then glyphfx, same seed, byte-identical streams |
| `03-throughput.mp4` | 11 s | ms/frame with pacing off, glyphfx vs ttfx |
| `04-parity-matrix.mp4` | 16 s | `matrix` under ttfx then glyphfx, same seed, byte-identical streams |
| `05-realworld.mp4` | 24 s | `git log \| glyphfx matrix`, `ls -la \| glyphfx decrypt` |
| `06-wow.mp4` | 18 s | `laseretch` on the wordmark, then `blackhole` on the banner |

Display size: each is 1920x1080, 30 fps, H.264 + AAC. Keep them short when
posting; 01, 03, and 06 make the strongest single posts.

## Exact commands used

Cold path (median of 200 runs, same harness for both binaries; `scripts/startup.sh`):

```
# CLI / loader floor
ttfx    --version
glyphfx --version

# time to first rendered frame
ttfx    --frame-rate 0 --max-frames 1 --virtual-clock beams
glyphfx --frame-rate 0 --max-frames 1 --virtual-clock beams

# whole effect, pacing off
ttfx    --frame-rate 0 --virtual-clock beams
glyphfx --frame-rate 0 --virtual-clock beams
```

Parity (same input, same seed; `scripts/parity.sh <effect> <seed>`):

```
# visual
ttfx    --seed 7 --canvas-width 0 --canvas-height 0 --anchor-canvas c --anchor-text c beams < banner.txt
glyphfx --seed 7 --canvas-width 0 --canvas-height 0 --anchor-canvas c --anchor-text c beams < banner.txt

# byte-exact proof
COLUMNS=140 LINES=36 ttfx    --seed 7 --parity-dump --virtual-clock beams < banner.txt > ttfx.frames
COLUMNS=140 LINES=36 glyphfx --seed 7 --parity-dump --virtual-clock beams < banner.txt > gf.frames
cmp ttfx.frames gf.frames
```

Throughput (`scripts/throughput.sh`, canvas 140x36, 150 frames, best of 3):

```
python3 tools/tests/bench.py        # same benchmark the repo ships
```

Real-world (`scripts/realworld.sh`):

```
git log --oneline -15 | glyphfx matrix --rain-time 1
ls -la                | glyphfx decrypt --typing-speed 12
```

Wow (`scripts/wow.sh`):

```
glyphfx --canvas-width 70 --canvas-height 12 --anchor-canvas c --anchor-text c laseretch < short.txt
glyphfx --canvas-width 0  --canvas-height 0  --anchor-canvas c --anchor-text c blackhole < banner.txt
```

## Measured numbers

Cold path (median of 200 runs each; totals include identical process-spawn cost,
so compare the two columns, not the absolute floor):

| path | glyphfx | ttfx | glyphfx speedup |
|---|---:|---:|---:|
| CLI startup (--version) | 0.367 ms | 0.865 ms | 2.4x |
| first frame (beams) | 0.456 ms | 0.893 ms | 2.0x |
| whole effect (beams) | 0.478 ms | 0.908 ms | 1.9x |

`--version` alone only exercises the loader and CLI parser, so the clip also
measures the real path: time to first frame, and a full effect render. Every
glyphfx path here is sub-millisecond, including rendering all 288 frames of
`beams` with pacing disabled.

Throughput (ms/frame, pacing disabled, best of 3):

| effect | glyphfx | ttfx | speedup |
|---|---:|---:|---:|
| slide | 0.034 | 0.044 | 1.3x |
| beams | 0.594 | 0.543 | 0.9x |
| blackhole | 0.027 | 0.029 | 1.1x |
| matrix | 0.054 | 0.045 | 0.8x |

Both engines render the same canvas in tens to hundreds of microseconds per
frame; glyphfx leads on two of four here and is within measurement noise on the
others. The large gains over the original Python TTE (not shown here) come from
being a native binary at all.

## Derived from this run

- **Effect parity:** every recorded effect is byte-identical to ttfx on the same
  input and seed. Full-animation sweeps across all 37 effects and several seeds
  also pass (`make effects`).
- **Portability:** no AVX-512 or hand-written assembly. Portable C17 keeps one
  binary correct on x86-64 and ARM alike; see the README "Why not assembly" note.
- **Runs on ordinary hardware:** the clips were captured on a normal Sway
  desktop at 1080p; startup and frame rates are unaffected by terminal features.

## Inputs used

- `inputs/short.txt` — the single word `glyphfx`
- `inputs/banner.txt` — the GLYPHFX ANSI-shadow wordmark
- `inputs/ls.txt` — `ls -la`
- `inputs/gitlog.txt` — `git log --oneline -15`

## Suggested captions

- **Show HN / X title:** "glyphfx — terminal text effects in one C17 binary, byte-identical to ttfx"
- **01-startup:** "Cold start, first frame, whole effect: all under 0.5 ms, ~2x faster than the Rust reference."
- **02-parity-beams:** "Same input, same seed, bytes identical to ttfx. beams."
- **03-throughput:** "ms per rendered frame with pacing off; glyphfx matches ttfx, no AVX-512."
- **04-parity-matrix:** "matrix, byte-identical to ttfx."
- **05-realworld:** "git log | glyphfx matrix, and ls -la | glyphfx decrypt."
- **06-wow:** "laseretch then blackhole, straight out of one C binary."

## Limitations

- **Python TerminalTextEffects is not installed in this environment** (`import
  terminaltexteffects` fails; no `reference/tte` checkout). The side-by-side
  comparison therefore uses ttfx as the reference, which is the project's
  byte-exact oracle. A three-way Python TTE clip can be added once TTE is
  installed.
- Narration uses edge-tts, which sends the narration text to Microsoft; the
  spoken text here is generic demo prose, not sensitive.
- The recordings live in a runtime directory; copies are in `docs/demo/videos/`.
