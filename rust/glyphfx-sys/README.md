# glyphfx-sys

Synchronized native ownership boundary for the glyphfx C17 text-effects engine.
Prefer the safe `glyphfx` crate for applications.

This package includes the complete engine source, its VERSION, MIT LICENSE and
NOTICE, and builds it locally with `cc`. No source downloads, system glyphfx
library, executable invocation or duplicate vendored engine tree are required.
A C17 compiler, POSIX platform and libm are required (tested on Linux).

The native shim uses virtual time and per-clip render state; Rust serializes
access to process-global native pools. No terminal output or wall-clock pacing
is performed. See the safe crate README for ownership and isolation details.
