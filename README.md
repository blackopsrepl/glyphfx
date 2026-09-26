# glyphfx

Terminal text effects as a standalone C17 binary. Pipe text in, pick an effect:

```sh
ls -la | glyphfx decrypt
cat banner.txt | glyphfx beams
fortune | glyphfx --random-effect
```

glyphfx is a terminal text-effects engine written in C17. It builds against
libc and libm only — no ncurses, no config parser, no RNG library — and all 37
effects, the animation engine, and the command-line surface are implemented
here. Given the same input, options, and seed as ttfx it emits byte-identical
frames and a byte-identical terminal stream; that compatibility is verified
mechanically in CI, not asserted.

## Status

Engine and effects are complete and checked against the ttfx binary:

- `make check` — pure-function, geometry, gradient, and RNG goldens.
- `make parity` — the M0 input/canvas/anchoring option matrix (306 cases).
- `make effects` — every effect's frame stream across seeds and configs.
- `tools/tests/cli_corpus.sh` — exit codes and stdout/stderr routing.
- `tools/tests/tty_compare.py` — the full pty prep+frames+teardown stream.
- `tools/tests/tty_signals.py` — SIGINT/SIGTERM/close/resize behavior.

Byte-exact comparison is pinned to Linux/glibc; builds and the unit tests also
run on macOS.

## Build

```sh
make            # build/glyphfx
make check      # C unit tests (pure-function, geometry, gradient, RNG goldens)
make effects    # byte-exact effect comparison against the ttfx binary
make parity     # byte-exact M0 option-matrix comparison
make debug      # ASan/UBSan build
make install    # PREFIX=/usr/local by default
make static     # static link (build/glyphfx-static)
```

The comparison oracle is a local ttfx checkout, fetched on demand and
gitignored:

```sh
tools/parity/fetch_reference.sh
```

`make effects` expects `reference/target/release/ttfx` unless `GLYPHFX_TTFX` is
set. A single effect runs with `tools/parity/run_effects.sh <effect>`.

## Options

Terminal options go before the effect name; effect options after it. Names,
defaults, metavars, choices, negative-value handling, and nargs match ttfx. The
hidden flags `--m0-dump`, `--parity-dump`, `--max-frames`, and `--virtual-clock`
are part of the test contract.

## License

MIT. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
