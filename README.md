# glyphfx

Terminal text effects as a standalone C17 binary. Pipe text in, pick an effect:

```sh
ls -la | glyphfx decrypt
cat banner.txt | glyphfx beams
fortune | glyphfx --random-effect
```

glyphfx is a pure-C17 implementation of the Rust binary
[ttfx](https://github.com/omacom/ttfx). It is a parity port: given the same
input, configuration, and seed, glyphfx emits byte-identical frames and a
byte-identical terminal stream. Behavior is specified by the ttfx binary, not
reinterpreted. libc and libm are the only dependencies.

## Status

The engine and all 37 effects are implemented and verified byte for byte
against the ttfx oracle, including the full CLI option surface, signal and
resize handling, and the terminal byte stream. The verification suites:

- `make check` — pure-function, geometry, gradient, and RNG goldens.
- `make parity` — the M0 input/canvas/anchoring option matrix.
- `make effects` — every effect's frame stream across seeds and configs.
- `tools/tests/cli_corpus.sh` — exit codes and stdout/stderr routing.
- `tools/tests/tty_compare.py` — the full pty prep+frames+teardown stream.
- `tools/tests/tty_signals.py` — SIGINT/SIGTERM/close/resize behavior.

Byte-exact parity is pinned to Linux/glibc; builds and the unit tests also run
on macOS.

## Build

```sh
make            # build/glyphfx
make check      # C unit tests (pure-function, geometry, gradient, RNG goldens)
make effects    # byte-exact effect parity against the ttfx oracle
make parity     # byte-exact M0 option-matrix parity
make debug      # ASan/UBSan build
```

The oracle is a local ttfx checkout, fetched on demand and gitignored:

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
