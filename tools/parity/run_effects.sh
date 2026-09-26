#!/usr/bin/env bash
# Generic effect frame-parity harness.
#
#   tools/parity/run_effects.sh <effect> [cases-file]
#
# Each case line:  <seed> | <terminal options> | <effect options>
# ('#' comments and blank lines are skipped.) Inputs come from
# tools/parity/inputs/*.txt. Frames are compared as ttfx's length-prefixed
# --parity-dump stream.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ORACLE="${GLYPHFX_TTFX:-$ROOT/reference/target/release/ttfx}"
SUBJECT="${GLYPHFX_BIN:-$ROOT/build/glyphfx}"
INPUTS_DIR="$ROOT/tools/parity/inputs"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

EFFECT="${1:?usage: run_effects.sh <effect> [cases-file]}"
CASES="${2:-$ROOT/tools/parity/cases/$EFFECT.txt}"
MAX_FRAMES="${GLYPHFX_MAX_FRAMES:-60}"

[ -x "$ORACLE" ] || { echo "oracle not found: $ORACLE" >&2; exit 2; }
[ -x "$SUBJECT" ] || { echo "subject not found: $SUBJECT (run make)" >&2; exit 2; }
[ -f "$CASES" ] || { echo "cases not found: $CASES" >&2; exit 2; }

export COLUMNS="${GLYPHFX_COLS:-40}"
export LINES="${GLYPHFX_LINES:-15}"

# Inputs whose bytes are not valid UTF-8 or not animatable are excluded.
INPUTS=(ascii.txt colored.txt tabs.txt ragged.txt spaces.txt trailing.txt csi_move.txt unicode.txt ignored_sgr.txt private.txt noeol.txt)

pass=0
fail=0
first=""

# Strip surrounding whitespace from a field.
trim() {
    local s="$1"
    s="${s#"${s%%[![:space:]]*}"}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "$s"
}

while IFS='|' read -r raw_seed raw_term raw_eff; do
    seed="$(trim "$raw_seed")"
    termopts="$(trim "$raw_term")"
    effopts="$(trim "$raw_eff")"
    case "$seed" in ''|\#*) continue ;; esac
    # shellcheck disable=SC2086
    for input in "${INPUTS[@]}"; do
        # shellcheck disable=SC2086
        "$ORACLE" --seed "$seed" --parity-dump --max-frames "$MAX_FRAMES" --virtual-clock $termopts "$EFFECT" $effopts \
            < "$INPUTS_DIR/$input" > "$TMP/oracle.out" 2> "$TMP/oracle.err"
        orc=$?
        # shellcheck disable=SC2086
        "$SUBJECT" --seed "$seed" --parity-dump --max-frames "$MAX_FRAMES" --virtual-clock $termopts "$EFFECT" $effopts \
            < "$INPUTS_DIR/$input" > "$TMP/subject.out" 2> "$TMP/subject.err"
        src=$?
        if [ "$orc" -ne 0 ] || [ "$src" -ne 0 ]; then
            fail=$((fail + 1))
            echo "FAIL (nonzero exit) seed=$seed input=$input term='$termopts' eff='$effopts' oracle=$orc subject=$src"
            [ -z "$first" ] && first="nonzero exit seed=$seed input=$input"
            continue
        fi
        if ! cmp -s "$TMP/oracle.out" "$TMP/subject.out"; then
            fail=$((fail + 1))
            line="seed=$seed input=$input term='$termopts' eff='$effopts'"
            echo "FAIL $line"
            if [ -z "$first" ]; then
                first="$line"
                echo "--- first divergence ---"
                python3 "$ROOT/tools/parity/differ.py" "$TMP/oracle.out" "$TMP/subject.out" || true
            fi
            continue
        fi
        pass=$((pass + 1))
    done
done < <(sed -e 's/#.*//' -e '/^[[:space:]]*$/d' "$CASES")

echo "$EFFECT parity: $pass passed, $fail failed"
if [ "$fail" -ne 0 ]; then
    [ -n "$first" ] && echo "first failure: $first"
    exit 1
fi
