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

The engine is complete: arena and character ids, the CSI input emulator,
canvas and anchoring, scenes with sync/eased stepping, path motion and events
with inline reentrant dispatch, particles, spanning trees, gradients, easing,
and the xoshiro256++ RNG.

Effects are ported one file at a time and checked byte for byte against the
ttfx oracle. Every effect with a case file in `tools/parity/cases/` is ported
and passing; `make effects` runs all of them. The remaining effects are still
being ported.

Not yet implemented: the remaining heavy effects (M5), CLI polish
(`--help`/`--print-completion`/`--random-effect` filtering), and the pty and
release-engineering suites.

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
