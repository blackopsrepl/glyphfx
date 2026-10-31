# glyphfx Rust API

`glyphfx` is a safe UI-agnostic pull API over the actual C17 engine.
`glyphfx-sys` builds the existing repository sources using `cc`; there is no
vendored engine copy. A C17 compiler and a POSIX platform with libm are required
(verified on Linux). No executable, subprocess, terminal writer, signal handler,
or wall-clock pacing is used. `EFFECTS` lists all 41 engine effects (the 37 ttfx
effects, then the four glyphfx originals in `ORIGINALS`); each runs with its
finite engine defaults. `burnout` is an exit effect: its text burns away.

```rust
let mut clip = glyphfx::Clip::new("beams", "Hello", 40, 10)?;
while let Some(ansi) = clip.next_frame() {
    // Full canvas: newline-separated rows, top first, with SGR styles.
    // Decode/render it in your UI and choose your own playback pacing.
}
assert!(clip.is_finished());
# Ok::<(), String>(())
```

Default seed is 1; `Clip::with_seed` chooses another deterministic stream.
`Clip::with_options` also sets where the text settles (`Anchor::Center`, …;
default `NorthWest`).
The virtual clock advances 1/60 second per emitted frame. Construction builds
simulation state, never prerenders. `is_finished` becomes true when a pull
observes completion; subsequent pulls are fused. Frames are owned Rust strings.

Zero dimensions, canvases over 65536 cells, empty/whitespace text, embedded NUL,
input over 1 MiB, unknown effects and engine input parsing errors return `Err`.
Canvas anchoring is northwest and independent of the host terminal dimensions.
The C engine places input codepoints in cells, rather than measuring graphemes;
ANSI consumers must honor Unicode display widths and clip each output row.
The API does not change engine input SGR handling (colors are supported; input
modifiers may be ignored by the engine). It preserves the output SGR stream.

## Isolation and ownership

Unsafe Rust is confined to `glyphfx-sys`, whose small ownership boundary copies
borrowed C strings while holding a process-wide mutex. The engine's intern and
visual pools are process-global and protected by that same lock. RNG state is
already stored in each `EngineCtx`. The private C shim swaps **the entire
RenderLog**, including allocation, pending entries and enable/overflow flags,
and rebinds its target terminal before every call. It detaches after every call
and frees the per-clip log on drop. Alternating clips and constructing/dropping
clips cannot leave a dangling render target. Clips are conservatively thread
local; separate threads may create independent clips safely. No direct engine
calls outside this boundary may run concurrently with it.

## Distribution

Both crates are registry-ready MIT packages. The repository root is the
`glyphfx-sys` package: its archive includes the existing engine source tree,
VERSION, LICENSE and NOTICE, plus the bridge and Rust ownership boundary.
The safe package contains Rust only and a versioned sys dependency. Neither
build downloads engine sources or relies on files outside its package root.
Publish `glyphfx-sys` before `glyphfx`; no publication is part of verification.
Applications can use `glyphfx = "0.4.1"` once both packages are published.

Until then, depend on the git repository at a `main` commit; Cargo discovers
`rust/glyphfx/Cargo.toml` without a `package` rename or subdirectory URL:

```toml
glyphfx = { git = "https://github.com/blackopsrepl/glyphfx.git", rev = "<FULL_COMMIT_SHA>" }
```

`tools/verify_rust_packages.py` checks the archives before publication: it runs
`cargo package -p glyphfx-sys`, unpacks the archive outside the checkout, builds
and tests it, then packages and tests the safe crate against the unpacked sys
archive through a verification-only `patch.crates-io` entry. The safe archive
keeps a plain registry version dependency.

## Verification

```sh
cargo test -j2 --workspace
cargo clippy -j2 --workspace --all-targets -- -D warnings
cargo package -j2 -p glyphfx-sys
# Once glyphfx-sys is published (or patched locally for verification):
cargo package -j2 -p glyphfx
# Verify both archives before sys is published (Python 3.12+):
python3 tools/verify_rust_packages.py
```

Tests check `EFFECTS` against the engine registry, run every effect through
actual engine completion, compare every frame against an interleaved second
execution, exercise independent threads and early drop, reject invalid inputs,
and run every effect on 1x1, 1x4, 4x1 and 2x2 canvases.
