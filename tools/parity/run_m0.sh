#!/usr/bin/env bash
# M0 parity: compare glyphfx's preprocessed first frame against the ttfx oracle
# byte for byte across the anchor/canvas/wrap/tab/existing-color matrix.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ORACLE="${GLYPHFX_TTFX:-$ROOT/reference/target/release/ttfx}"
SUBJECT="${GLYPHFX_BIN:-$ROOT/build/glyphfx}"
INPUTS_DIR="$ROOT/tools/parity/inputs"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ ! -x "$ORACLE" ]; then
    echo "oracle not found: $ORACLE" >&2
    echo "build it with: (cd reference && cargo build --release)" >&2
    exit 2
fi
if [ ! -x "$SUBJECT" ]; then
    echo "subject not found: $SUBJECT (run make)" >&2
    exit 2
fi

export COLUMNS=40
export LINES=15

pass=0
fail=0
first_report=""

# Each entry is a set of extra terminal options (may be empty).
CONFIGS=(
    ""
    "--canvas-width 30 --canvas-height 12"
    "--canvas-width 0 --canvas-height 0"
    "--ignore-terminal-dimensions"
    "--anchor-canvas nw --anchor-text c"
    "--anchor-canvas se --anchor-text n"
    "--anchor-canvas c --anchor-text se"
    "--anchor-text ne"
    "--wrap-text --canvas-width 12"
    "--wrap-text --canvas-height 6"
    "--tab-width 8"
    "--tab-width 2"
    "--existing-color-handling always"
    "--existing-color-handling dynamic"
    "--xterm-colors"
    "--no-color"
    "--xterm-colors --existing-color-handling always"
)

for input in "$INPUTS_DIR"/*.txt; do
    name="$(basename "$input")"
    for cfg in "${CONFIGS[@]}"; do
        # shellcheck disable=SC2086
        "$ORACLE" --m0-dump $cfg < "$input" > "$TMP/oracle.out" 2> "$TMP/oracle.err"
        orc=$?
        # shellcheck disable=SC2086
        "$SUBJECT" --m0-dump $cfg < "$input" > "$TMP/subject.out" 2> "$TMP/subject.err"
        src=$?
        if [ "$orc" -ne "$src" ]; then
            fail=$((fail + 1))
            line="exit mismatch input=$name cfg='$cfg' oracle=$orc subject=$src"
            echo "FAIL $line"
            [ -z "$first_report" ] && first_report="$line"
            continue
        fi
        if ! cmp -s "$TMP/oracle.out" "$TMP/subject.out"; then
            fail=$((fail + 1))
            line="stdout mismatch input=$name cfg='$cfg'"
            echo "FAIL $line"
            if [ -z "$first_report" ]; then
                first_report="$line"
                echo "--- first divergence ($name / $cfg) ---"
                python3 "$ROOT/tools/parity/differ.py" "$TMP/oracle.out" "$TMP/subject.out" || true
            fi
            continue
        fi
        pass=$((pass + 1))
    done
done

echo "m0 parity: $pass passed, $fail failed"
if [ "$fail" -ne 0 ]; then
    [ -n "$first_report" ] && echo "first failure: $first_report"
    exit 1
fi
