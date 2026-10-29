# glyphfx Rust API

`glyphfx` is a safe UI-agnostic pull API over the actual C17 engine.
`glyphfx-sys` builds the existing repository sources using `cc`; there is no
vendored engine copy. A C17 compiler and a POSIX platform with libm are required
(verified on Linux). No executable, subprocess, terminal writer, signal handler,
or wall-clock pacing is used. All 37 effects use their finite engine defaults.

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

## Distribution and Planner integration

Both crates are registry-ready MIT packages. The repository root is the
`glyphfx-sys` package: its archive includes the existing engine source tree,
VERSION, LICENSE and NOTICE, plus the bridge and Rust ownership boundary.
The safe package contains Rust only and a versioned sys dependency. Neither
build downloads engine sources or relies on files outside its package root.
Publish `glyphfx-sys` before `glyphfx`; no publication is part of verification.
Applications can use `glyphfx = "0.1.0"` once both packages are published.

The previous nested sys manifest used `../../src` and `publish = false`.
That worked from a git checkout but could not produce a self-contained registry
archive. Moving the sys manifest to the existing engine root fixes that packaging
regression without copying the C tree; the build script now resolves only paths
inside its package. Both manifests carry registry metadata and MIT attribution.

To verify before publication, first run `cargo package -p glyphfx-sys`, then
unpack its generated `.crate` archive outside the checkout. Run the safe
package command with `--config 'patch.crates-io.glyphfx-sys.path="/absolute/path/to/unpacked/glyphfx-sys-0.1.0"'`.
The patch is verification-only: the safe archive retains a normal registry
version dependency. Unpack that archive alongside sys and run its tests with
the same patch to verify both independent package outputs.

After publishing the reusable API commit, Planner replaces its temporary path:

```toml
# Remove: glyphfx = { path = "/srv/lab/hack/glyphfx-rust-api/rust/glyphfx" }
glyphfx = { git = "https://github.com/blackopsrepl/glyphfx.git", rev = "<FULL_API_COMMIT_SHA>" }
```

Cargo discovers `rust/glyphfx/Cargo.toml` in the git repository; no `package`
rename or subdirectory URL is needed. Then run `cargo update -p glyphfx` and
commit the resulting Cargo.lock. If publishing only to the local forge, use
`http://vigilance:3002/blackopsrepl/glyphfx.git` instead. The Planner adapter
requires the parent integration to add `pub mod glyphfx;` to `src/lib.rs`.
Its tests also compile the adapter directly so the branch is independently
testable before that export lands. The adapter returns zero-origin, fully
styled ratatui buffers and contains neither C nor unsafe Rust.

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

Tests run every effect through actual engine completion, compare every frame
against an interleaved second execution, exercise independent threads and early
drop, reject invalid inputs, and run all 37 effects on 1x1, 1x4, 4x1 and 2x2
canvases. Planner additionally checks real styled buffers and Unicode output,
plus parser fixtures for RGB/indexed/basic foreground/background, every ratatui
modifier, resets, combining characters, emoji/flags/ZWJ, clipping and padding.
