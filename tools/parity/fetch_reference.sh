#!/usr/bin/env bash
# Fetch the ttfx oracle into reference/ (gitignored). The parity harness and
# run_m0.sh compare glyphfx against this binary.
#
# Usage: tools/parity/fetch_reference.sh [ref]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEST="$ROOT/reference"
URL="${TTFX_URL:-https://github.com/omacom/ttfx.git}"
REF="${1:-${TTFX_REF:-54d21f046f22512b113056a1964077d7b7bf04cc}}"

if [ -d "$DEST/.git" ]; then
    echo "reference/ already present; fetching $REF"
    git -C "$DEST" fetch --depth 1 origin "$REF"
    git -C "$DEST" checkout --detach FETCH_HEAD
else
    git clone "$URL" "$DEST"
    git -C "$DEST" checkout --detach "$REF"
fi

echo "building oracle..."
cargo build --release --manifest-path "$DEST/Cargo.toml"
echo "oracle: $DEST/target/release/ttfx"
