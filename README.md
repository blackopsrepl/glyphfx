# glyphfx

Terminal text effects as a standalone C17 binary. Pipe text in, pick an effect:

```sh
ls -la | glyphfx decrypt
cat banner.txt | glyphfx beams
fortune | glyphfx --random-effect
```

glyphfx is a from-scratch implementation in pure C of the Rust binary
[ttfx](https://github.com/omacom/ttfx), which is itself a port of
[TerminalTextEffects](https://github.com/ChrisBuilds/terminaltexteffects). It is
a parity port: given the same input, configuration, and seed, glyphfx emits
byte-identical frames and a byte-identical terminal stream. behavior is
specified against the ttfx binary, not reinterpreted.

## Status

This repository is being built in the milestone order described in the project
plan. Implemented and mechanically verified so far:

- **M0 — skeleton + input pipeline.** Table-driven CLI for the root and terminal
  options, strict UTF-8 input, the CSI-only input emulator, canvas + anchoring,
  fill characters, neighbors, renderer, and the tty writer. Exit criterion met:
  `glyphfx --m0-dump` matches `ttfx --m0-dump` byte for byte across the
  anchor/canvas/wrap/tab/existing-color matrix (306 cases).
- **M1 — engine core (partial).** Ordered map, geometry, gradient generation,
  RNG, pycompat, xterm palette, strict UTF-8, clock, plus the animation scenes,
  event tables with inline dispatch, active-character set, and the EngineCtx
  stepping model. Pure-function goldens are taken from ttfx's own fixtures.
  Easing (`make_easing`) and the eased/sync scene paths are not ported yet, and
  path-based motion is stubbed; they land with the effects that need them.
- **M2 — parity harness + first effect.** The generic effect harness plus
  `randomsequence`, verified byte for byte against the ttfx oracle (frames and
  full terminal stream). `make parity` runs the M0 matrix; the effect harness is
  `tools/parity/run_effects.sh randomsequence`.

Not yet implemented: the remaining 36 effects and their full option surface
(M3–M5), path motion and easing (M3+), and the CLI/runtime/release polish
(M6–M8). Invoking an unported effect reports it is not yet available.


## Build

libc and libm only; no ncurses, no getopt wrapper, no config parser, no RNG
library.

```sh
make            # build/glyphfx
make check      # C unit tests (pure-function and RNG goldens)
make parity     # byte-exact M0 comparison against the ttfx oracle
make debug      # ASan/UBSan build
```

The oracle is a local ttfx checkout, fetched on demand and gitignored:

```sh
tools/parity/fetch_reference.sh
```

`make parity` expects `reference/target/release/ttfx` unless `GLYPHFX_TTFX` is
set.

## Options

Terminal options go before the effect name; effect options after it. Names and
defaults match ttfx. The hidden flags `--m0-dump`, `--parity-dump`,
`--max-frames`, and `--virtual-clock` are part of the test contract.

## License

MIT. glyphfx originates none of the effect art; see [LICENSE](LICENSE) and
[NOTICE](NOTICE). The upstream copyrights are preserved.
