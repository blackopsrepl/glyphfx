#!/usr/bin/env bash
# Parity demo: same input + seed under ttfx and glyphfx, then the byte-identical
# frame-stream proof. The canvas is the full terminal so the effect reads well.
set -u
cd /srv/lab/hack/glyphfx

TTFX=reference/target/release/ttfx
GF=build/glyphfx
EFFECT="${1:-beams}"
SEED="${2:-7}"
IN="${3:-/tmp/opencode/demo/inputs/banner.txt}"
EXTRA=("${@:4}")
COLS=140
ROWS=36

green=$'\033[1;38;2;16;185;129m'
orange=$'\033[1;38;2;255;130;80m'
bold=$'\033[1m'
dim=$'\033[2m'
red=$'\033[1;31m'
reset=$'\033[0m'

clear
printf '\n  %sparity%s  ·  effect %s%s%s  ·  seed %s  ·  same input\n\n' \
    "$bold" "$reset" "$bold" "$EFFECT" "$reset" "$SEED"

printf '  %sttfx%s %s(Rust reference)%s\n\n' "$orange" "$reset" "$dim" "$reset"
"$TTFX" --seed "$SEED" --canvas-width 0 --canvas-height 0 --anchor-canvas c --anchor-text c "$EFFECT" "${EXTRA[@]+"${EXTRA[@]}"}" <"$IN"

printf '\n\n  %sglyphfx%s %s(C17 port)%s\n\n' "$green" "$reset" "$dim" "$reset"
"$GF" --seed "$SEED" --canvas-width 0 --canvas-height 0 --anchor-canvas c --anchor-text c "$EFFECT" "${EXTRA[@]+"${EXTRA[@]}"}" <"$IN"

COLUMNS=$COLS LINES=$ROWS "$TTFX" --seed "$SEED" --parity-dump --virtual-clock "$EFFECT" "${EXTRA[@]+"${EXTRA[@]}"}" <"$IN" >/tmp/ttfx.frames 2>/dev/null
COLUMNS=$COLS LINES=$ROWS "$GF"   --seed "$SEED" --parity-dump --virtual-clock "$EFFECT" "${EXTRA[@]+"${EXTRA[@]}"}" <"$IN" >/tmp/gf.frames   2>/tmp/gf.err
frames=$(sed -n 's/^frames=//p' /tmp/gf.err | head -1)
bytes=$(stat -c%s /tmp/gf.frames)

printf '\n\n'
if cmp -s /tmp/ttfx.frames /tmp/gf.frames; then
    printf '  %sframe stream%s  ttfx == glyphfx :  %sBYTE-IDENTICAL%s   %s frames, %s bytes\n\n' \
        "$bold" "$reset" "$green" "$reset" "${frames:-?}" "$bytes"
else
    printf '  %sframe stream%s  ttfx == glyphfx :  %sDIFFER%s\n\n' "$bold" "$reset" "$red" "$reset"
fi
