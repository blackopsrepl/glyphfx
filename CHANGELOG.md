# Changelog

All notable changes to glyphfx are documented here. This file is generated from conventional commits by commit-and-tag-version; do not edit by hand.

## [0.3.2](https://github.com/blackopsrepl/glyphfx/compare/v0.3.1...v0.3.2) (2026-09-27)

### Bug Fixes

* **animation:** reset the eased step when a scene resets ([02d5a36](https://github.com/blackopsrepl/glyphfx/commit/02d5a36b23be1008894909c23106d1a383e77c4b))

## [0.3.1](https://github.com/blackopsrepl/glyphfx/compare/v0.3.0...v0.3.1) (2026-09-27)

### Performance

* **engine:** resolve the activation path/scene with one lookup ([e1c8421](https://github.com/blackopsrepl/glyphfx/commit/e1c8421d360350e472e3b794994dcc6cf45b00a3))
* **events:** build caller keys from cached name handles ([60cc199](https://github.com/blackopsrepl/glyphfx/commit/60cc199bba774f6c8bb5cbfae28b8c7fe3a40288))
* **events:** compare the waypoint name handle before coordinates ([5337c38](https://github.com/blackopsrepl/glyphfx/commit/5337c3817a34af0d82333da01e4bd1d034485c03))
* **events:** identify event keys by interned name handles ([dc42eae](https://github.com/blackopsrepl/glyphfx/commit/dc42eae802823007be733529ae716fbd62197b9f))
* **render:** drive the row cache from a dirty-row bitmap ([f7d02ed](https://github.com/blackopsrepl/glyphfx/commit/f7d02edc8421ff3645d5ca2be5c3f81834fcd194))
* **tools:** bound the benchmark gate ([376178e](https://github.com/blackopsrepl/glyphfx/commit/376178ef7fcb9a4c5e1faeb82c06260411be6af8))

### Documentation

* add the terminal demo suite ([08773af](https://github.com/blackopsrepl/glyphfx/commit/08773afcfe0331b1dbe75e3f70de7be06e91510b))

## [0.3.0](https://github.com/blackopsrepl/glyphfx/compare/v0.2.1...v0.3.0) (2026-09-27)

### Performance

* **render:** row buffers with a handle-grid clean test ([c1b1239](https://github.com/blackopsrepl/glyphfx/commit/c1b1239dad8a77cdf1199802bcae9d2bdac8a541))
* **rng:** inline the random helpers and fix the clz width at n == 1 ([06fd8ee](https://github.com/blackopsrepl/glyphfx/commit/06fd8ee0d34b1527e6e705a9c3a33e2daa4f92b3))
* **scene:** index scene frames by head instead of a played-frames deque ([dc7f057](https://github.com/blackopsrepl/glyphfx/commit/dc7f057a1e106550f5b9effaaca696a547d613dc))
* **tools:** add the asm-target benchmark gate ([2a87417](https://github.com/blackopsrepl/glyphfx/commit/2a8741762ce3fedbc6c41bde20f3a8bec188911c))
* **visual:** add the offset-addressed visual pool ([5c3a645](https://github.com/blackopsrepl/glyphfx/commit/5c3a645cc1495cfc241a192ad4900bfa4e946d7c))
* **visual:** address visuals as pool handles end to end ([f1dffe9](https://github.com/blackopsrepl/glyphfx/commit/f1dffe9c900848600ceef7dbe8e94a14a525090f))

## [0.2.1](https://github.com/blackopsrepl/glyphfx/compare/v0.2.0...v0.2.1) (2026-09-27)

### Performance

* **animation:** intern visuals on a packed appearance key ([8d38052](https://github.com/blackopsrepl/glyphfx/commit/8d38052472e170059cf6b0af42196ddf88d585b5))
* **animation:** memoize the eased frame-index sequence ([9bdb08b](https://github.com/blackopsrepl/glyphfx/commit/9bdb08b14aedaa2ae4989e62dbbb8023f0a4d50a))
* **motion:** resolve the active path from its cached slot ([46f2d26](https://github.com/blackopsrepl/glyphfx/commit/46f2d26aa371b8d1bd7a167c7e9edc5c953ccd87))

## [0.2.0](https://github.com/blackopsrepl/glyphfx/compare/v0.1.9...v0.2.0) (2026-09-27)

### Performance

* **render:** emit four cells per iteration in the row writer ([d38d044](https://github.com/blackopsrepl/glyphfx/commit/d38d0449bca9088d2c6ccb0af41f11d53c1bd5ea))
* **render:** emit frames as one iovec per row instead of an assembled string ([e88a1c7](https://github.com/blackopsrepl/glyphfx/commit/e88a1c7c573b21cf72ec28293e205ce8042359ca))
* **render:** serialize from a handle grid instead of the character arena ([4f2a933](https://github.com/blackopsrepl/glyphfx/commit/4f2a933798c59e18fc99d3490ca4dc1c64212b00))

### Tests

* **render:** teach the row verifier about stored newlines ([71aeaef](https://github.com/blackopsrepl/glyphfx/commit/71aeaef2693676f0b556c94674b117f4b486af10))

## [0.1.9](https://github.com/blackopsrepl/glyphfx/compare/v0.1.8...v0.1.9) (2026-09-27)

### Performance

* **matrix:** share resolve frames by symbol and final color ([8988c0b](https://github.com/blackopsrepl/glyphfx/commit/8988c0b433c6fc3ba2f3edfc05d797543a59ac3e))
* **render:** maintain the cell grid incrementally from a mutation log ([22cf6aa](https://github.com/blackopsrepl/glyphfx/commit/22cf6aad0b86027dffa9b067a2131e050f233e8a))

## [0.1.8](https://github.com/blackopsrepl/glyphfx/compare/v0.1.7...v0.1.8) (2026-09-27)

### Performance

* **laseretch:** share the spark cooling frames across sparks ([9865775](https://github.com/blackopsrepl/glyphfx/commit/9865775b53084d6c2768d2e9c7bb3c943b3e498a))
* **render:** copy unchanged rows from cached bytes ([4b9a46e](https://github.com/blackopsrepl/glyphfx/commit/4b9a46e389ee43140690e9ece138f4a89f0a7349))
* **render:** gate the row cache by painted density, abandon hopeless probes ([037e1fd](https://github.com/blackopsrepl/glyphfx/commit/037e1fd4e0d1ab42699300914e1bbd18b9d2ed79))

## [0.1.7](https://github.com/blackopsrepl/glyphfx/compare/v0.1.6...v0.1.7) (2026-09-27)

### Performance

* **bubbles:** keep each bubble's circle trig instead of recomputing it ([3624c08](https://github.com/blackopsrepl/glyphfx/commit/3624c08753b805bee5b82cfdcfd09d415e9c9bfe))
* **burn:** share the smoke fade frames across particles ([68d343d](https://github.com/blackopsrepl/glyphfx/commit/68d343daedf4ecfb5964d8d716caa965b6ee582b))
* **print:** memoize head scenes by symbol and final color ([000427e](https://github.com/blackopsrepl/glyphfx/commit/000427e7696250f062d9837d8da7bd5d8bb3cc34))

## [0.1.6](https://github.com/blackopsrepl/glyphfx/compare/v0.1.5...v0.1.6) (2026-09-27)

### Performance

* **animation:** restyle the live visual in place when uniquely owned ([858e7bb](https://github.com/blackopsrepl/glyphfx/commit/858e7bb977621c5cb545012a0f4becced6372031))
* **motion:** intern waypoint identity strings ([2a10272](https://github.com/blackopsrepl/glyphfx/commit/2a102726215fc7d77296ea5ec5e1a94fcef333fb))
* **ordmap:** index large maps for O(1) string lookup ([0548a0f](https://github.com/blackopsrepl/glyphfx/commit/0548a0fd5c90881bc2d3b605bc538f332fcc4caf))
* **render:** one cell array with an empty sentinel instead of epoch stamps ([2101c14](https://github.com/blackopsrepl/glyphfx/commit/2101c14b0c3e7fc728dac703d60ab21fab2736ad))
* **smoke:** share the template smoke scene and keyed paint runs ([630611b](https://github.com/blackopsrepl/glyphfx/commit/630611b87c908e44fd2e9a6b5a30b3ad245d01d1))

### Documentation

* target the ttfx asm engine, retire the rust-parity framing ([9a4bf6b](https://github.com/blackopsrepl/glyphfx/commit/9a4bf6b104b69748af7eec75282d7dade6ee3c14))

## [0.1.5](https://github.com/blackopsrepl/glyphfx/compare/v0.1.4...v0.1.5) (2026-09-26)

### Performance

* **events:** keep non-callback action ids on the stack ([1804f03](https://github.com/blackopsrepl/glyphfx/commit/1804f03c4afacaeae05da12cdd20d8878343706c))
* **render:** store formatted symbols inline and copy a fixed block ([3ccc311](https://github.com/blackopsrepl/glyphfx/commit/3ccc3116571ff81241a17bdd23587d8133499bfe))
* **strbuf:** inline the hot small appends ([b34f045](https://github.com/blackopsrepl/glyphfx/commit/b34f0456506b3a1b164eb9cbbab2f270d23ae2f1))

## [0.1.4](https://github.com/blackopsrepl/glyphfx/compare/v0.1.3...v0.1.4) (2026-09-26)

### Performance

* **animation:** hash and compare visual keys directly ([b9a5939](https://github.com/blackopsrepl/glyphfx/commit/b9a5939c5824f21fd1a814603fc8b8984fac8b1e))
* **events:** reuse the caller id hash ([b29e306](https://github.com/blackopsrepl/glyphfx/commit/b29e30669ad2326994a4dfb3fbfba05f5d0ef097))
* **rng:** inline the xoshiro primitives ([4a36edb](https://github.com/blackopsrepl/glyphfx/commit/4a36edb1e64a72b56a6c8289dc8ab71bd31c8ee7))

## [0.1.3](https://github.com/blackopsrepl/glyphfx/compare/v0.1.2...v0.1.3) (2026-09-26)

### Performance

* **engine:** stamp the render grid instead of clearing it ([2d14816](https://github.com/blackopsrepl/glyphfx/commit/2d1481653816eeb547a78610bb0ebfcda73c4eac))
* **motion:** resolve the active path once per step ([5b87c7d](https://github.com/blackopsrepl/glyphfx/commit/5b87c7dd673da0b5f3af3f5391b1b5e59fc90664))

### CI

* **perf:** add a fail-closed performance regression gate ([2b34689](https://github.com/blackopsrepl/glyphfx/commit/2b346892fcecf7146dba0716edc8e2ccdb97a456))
* **perf:** drop the stale tolerance wording ([ff29840](https://github.com/blackopsrepl/glyphfx/commit/ff29840f760e62eb6482e97e3b78c82e08078e29))
* **perf:** gate at zero tolerance ([eb8d60f](https://github.com/blackopsrepl/glyphfx/commit/eb8d60f4655cfb501145e415845e4ce01ffb8f35))
* **perf:** remove the regression tolerance entirely ([99693ca](https://github.com/blackopsrepl/glyphfx/commit/99693ca35e56aa662f7157d3e4b2ccd6fbfc2969))
* **perf:** widen the gate's noise budget to 15%+30ms ([cca1b7b](https://github.com/blackopsrepl/glyphfx/commit/cca1b7b3918573a8602a98e48bcee4d4a25343a1))

### Documentation

* **readme:** full 37-effect performance matrix and repo facts ([ab9fd35](https://github.com/blackopsrepl/glyphfx/commit/ab9fd3562dc53ed4a2b7ad445c52d6600b61ed90))
* refresh the 37-effect performance matrix ([1b01a2e](https://github.com/blackopsrepl/glyphfx/commit/1b01a2e3d047e01e03b4e34c38aa72d21238b5af))

## [0.1.2](https://github.com/blackopsrepl/glyphfx/compare/v0.1.1...v0.1.2) (2026-09-26)

### Bug Fixes

* **demo:** cap the frame budget to the reference gallery's pace ([fbc3128](https://github.com/blackopsrepl/glyphfx/commit/fbc31287be9b38e1aabe19be45568de0350f3eb3))
* **demo:** render block glyphs as rectangles ([f5fef8b](https://github.com/blackopsrepl/glyphfx/commit/f5fef8b0deefdfdd7e6f43bce5020d3134cbe390))
* **demo:** sample the whole effect animation ([05e6e4f](https://github.com/blackopsrepl/glyphfx/commit/05e6e4feccfa8feced19311110c92a7c1151accb))
* **demo:** tighten canvas, type, and palette ([d037020](https://github.com/blackopsrepl/glyphfx/commit/d037020145953f571f0ae379e577b6d898acf493))

### Performance

* **animation:** evict pooled visuals and stop pooling appearances ([db25e57](https://github.com/blackopsrepl/glyphfx/commit/db25e57b645a703ad6ebbdb7355d167febd55fe3))
* **animation:** intern visuals in a process-wide pool ([a8e1ae5](https://github.com/blackopsrepl/glyphfx/commit/a8e1ae50837036c92e0ae31e2ed1b9c6098b7ca6))
* **animation:** keep a borrowed active-scene pointer ([e980398](https://github.com/blackopsrepl/glyphfx/commit/e9803988275d203d6c9dbe8d2587464519009d6f))
* **engine:** resolve the event entry once and reuse the tick snapshot ([ed7b94f](https://github.com/blackopsrepl/glyphfx/commit/ed7b94f550e73c3dcb2eb28465f08f8dc5dc6978))
* **engine:** use a packed bitmap for the active-character set ([d7a81db](https://github.com/blackopsrepl/glyphfx/commit/d7a81db68325e150b3cb43bb9241e1c01c370fa5))
* **events:** hash caller keys before strcmp ([6d9bc11](https://github.com/blackopsrepl/glyphfx/commit/6d9bc11859ffe15e411bb01368195388d4c00dd7))
* **ordmap:** compare a cached key hash before strcmp ([f698322](https://github.com/blackopsrepl/glyphfx/commit/f69832267279264557f3d35f9367a2ca415be7fe))
* **ordmap:** keep small maps inline ([35be174](https://github.com/blackopsrepl/glyphfx/commit/35be174c9afdbd44df70bf46a2b490653f7c5476))

### Build

* compile at -O3 ([2e92a7e](https://github.com/blackopsrepl/glyphfx/commit/2e92a7eb034cd552ea91e27e8ea2d852dbdbc351))
* give the Makefile the SolverForge treatment ([4d38c6a](https://github.com/blackopsrepl/glyphfx/commit/4d38c6ab175854c287de0b495c88f5f272832553))

### Documentation

* **readme:** add a why-not-assembly note ([dd544dc](https://github.com/blackopsrepl/glyphfx/commit/dd544dc00360d3c0a6f8585a506cb2fe7d7401ee))
* **readme:** introduce the GlyphFX mascot ([12a516a](https://github.com/blackopsrepl/glyphfx/commit/12a516a3e4f9d7e5e438ad2d920c6a1f559e2cd8))
* **readme:** regenerate the effect gifs ([ced9a19](https://github.com/blackopsrepl/glyphfx/commit/ced9a19c78d5d85a84a2421bc634c466b919cd97))
* **readme:** regenerate the effect gifs on the reference pace ([6fe056f](https://github.com/blackopsrepl/glyphfx/commit/6fe056fd113a02f4bf8673b2b4e57754f014219a))
* tighten README attribution, links, and layout ([5a1cfef](https://github.com/blackopsrepl/glyphfx/commit/5a1cfefedcd9e9a82034d49fe71b6de588fa8fea))
* use a legible block banner and make wipe the hero ([0d5f32d](https://github.com/blackopsrepl/glyphfx/commit/0d5f32db94c69b86672db88e9b72c569aee1e4fd))

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
