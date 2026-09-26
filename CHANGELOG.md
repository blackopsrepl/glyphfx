# Changelog

All notable changes to glyphfx are documented here. This file is generated from conventional commits by commit-and-tag-version; do not edit by hand.

## [0.1.1](https://github.com/blackopsrepl/glyphfx/compare/v0.1.0...v0.1.1) (2026-09-26)

### Bug Fixes

* **ci:** define SIGWINCH for macOS and update the oracle toolchain ([47c88f9](https://github.com/blackopsrepl/glyphfx/commit/47c88f9008cff01bf3494e46c5661596da0042fd))

### Performance

* share visuals, reuse frame buffers, and skip exit teardown ([f47c25b](https://github.com/blackopsrepl/glyphfx/commit/f47c25b15a15f564400d7fcf5e0276f53060dd9c))

### Documentation

* add rendered effect GIFs and a README at ttfx's bar ([76d9e6c](https://github.com/blackopsrepl/glyphfx/commit/76d9e6c0e3dbb8ff6aea6d32a0d1e7286c7cd5c2))
* describe glyphfx on its own terms ([c2ebc02](https://github.com/blackopsrepl/glyphfx/commit/c2ebc02ede9a00fa382624fd6d958679248f89d8))
* explain why glyphfx stays in portable C ([f40a532](https://github.com/blackopsrepl/glyphfx/commit/f40a5324928d67083173e698998050039590f7cc))
* restructure README to ttfx's layout ([01de6d5](https://github.com/blackopsrepl/glyphfx/commit/01de6d51c09ac0804f4db42c81f3943002dc75f8))

## 0.1.0 (2026-09-26)

### Features

* **cli:** full effect option kinds and a weak-symbol effect registry ([f4feef2](https://github.com/blackopsrepl/glyphfx/commit/f4feef2926c9deb4a78c4ca0e23a23599e0fe638))
* **effects:** beams, rings (wave 2) ([4516d2b](https://github.com/blackopsrepl/glyphfx/commit/4516d2b80243db316be3d0c98fa33c6024d81ef9))
* **effects:** bouncyballs, errorcorrect, colorshift, highlight ([21548df](https://github.com/blackopsrepl/glyphfx/commit/21548dfefaecdd9f04ca8a2988644d598cecfb95))
* **effects:** burn, smoke, laseretch (wave 3) ([24e46b9](https://github.com/blackopsrepl/glyphfx/commit/24e46b907211b3bb2160b1d883d5d5ad1d7d15b0))
* **effects:** expand, slice, scattered, pour (wave 1) ([17ea18a](https://github.com/blackopsrepl/glyphfx/commit/17ea18aaa42ea7b24dea89eff80d036eb8d58d80))
* **effects:** matrix (wave 3) ([60d2489](https://github.com/blackopsrepl/glyphfx/commit/60d2489f1e1776a1ce3321e7a68d1befaea298f3))
* **effects:** orbittingvolley, binarypath (wave 2 complete) ([1007398](https://github.com/blackopsrepl/glyphfx/commit/100739846640a1a77549d88f7f51a263edcf1a6b))
* **effects:** overflow, unstable, crumble, blackhole (wave 2) ([36822e8](https://github.com/blackopsrepl/glyphfx/commit/36822e83764b83401a38d2c5419be102f0a85ae6))
* **effects:** slide, middleout, spray, rain (wave 1) ([d102d90](https://github.com/blackopsrepl/glyphfx/commit/d102d9094fc3d0345f9f288d1c2165da611dc37b))
* **effects:** swarm, spotlights, fireworks, bubbles (wave 2) ([6b9af44](https://github.com/blackopsrepl/glyphfx/commit/6b9af448b41eb9b9540376942f1ff4e2daff6a6a))
* **effects:** sweep, waves, decrypt, print (wave 2) ([e51eb96](https://github.com/blackopsrepl/glyphfx/commit/e51eb9628a28242aece4d909cfd923302105ba94))
* **effects:** synthgrid (wave 3) ([4fe9900](https://github.com/blackopsrepl/glyphfx/commit/4fe9900c5b646fb2fdf5f93c90e3d197f408e772))
* **effects:** thunderstorm (wave 3 complete) ([dfe446e](https://github.com/blackopsrepl/glyphfx/commit/dfe446ecd3e4ac737d194769b9e8b5720d6eff51))
* **effects:** vhstape (wave 3) ([3085c96](https://github.com/blackopsrepl/glyphfx/commit/3085c96e01eee7b33cc1e9d40e3eeead3b8d2694))
* **effects:** wipe plus the shared sequence easer ([b018c51](https://github.com/blackopsrepl/glyphfx/commit/b018c51ce247d6f3f626da87dda227ec82ccce31))
* **engine:** easing, path motion, particles, spanning trees, full stepping ([c832e05](https://github.com/blackopsrepl/glyphfx/commit/c832e05b4cbf9b80269fbc8cdeb31a92b766b438))
* **engine:** M1 core, M2 effect pipeline, and randomsequence ([ecf9f7b](https://github.com/blackopsrepl/glyphfx/commit/ecf9f7b38c1120077888f2aa6379ca97839a6549))
* **m0:** input emulator, canvas, renderer, CLI, and --m0-dump ([7fc37a6](https://github.com/blackopsrepl/glyphfx/commit/7fc37a64561803fd91079243eb4a6343ad4a43fa))
* **runtime:** signals, resize, lost-terminal, and CLI polish ([cd5d366](https://github.com/blackopsrepl/glyphfx/commit/cd5d3661060c44c0b8c85f06da51b697eb097a38))
* **utils:** geometry, ordered map, and gradient generation ([0e04b6b](https://github.com/blackopsrepl/glyphfx/commit/0e04b6beff47b03eba0e0490aab98fb6d5c640bf))
* **utils:** pure-function core with oracle-pinned goldens ([3e6e998](https://github.com/blackopsrepl/glyphfx/commit/3e6e9983f64ed7e4f0836cfa15e288cfde036ecf))

### Bug Fixes

* **cli:** free the parsed effect config on every exit path ([15edb50](https://github.com/blackopsrepl/glyphfx/commit/15edb50b5d16175d9717ddb58504957fa1cc2a20))
* **parity:** make the harness truthful, and fix two real bugs it hid ([d6e19e3](https://github.com/blackopsrepl/glyphfx/commit/d6e19e3449778b787565903ab8ab0ef846c26960))

### Tests

* **parity:** M0 option-matrix harness against the ttfx oracle ([1c44c4b](https://github.com/blackopsrepl/glyphfx/commit/1c44c4b8315142957d6953fad0d7efd249ae8a1f))

### Documentation

* describe glyphfx, build, and milestone status ([dd7203e](https://github.com/blackopsrepl/glyphfx/commit/dd7203e3d8ec24fc5bb98c82620d1e245d335621))
* record M0-M2 completion and the easing/motion gaps ([82a882b](https://github.com/blackopsrepl/glyphfx/commit/82a882b60f9ff568560abf688cd35ebe5950a44b))
* state the port factually in README and NOTICE ([bc1f63b](https://github.com/blackopsrepl/glyphfx/commit/bc1f63bafee97faa4210728e67072ffc6da5c052))
