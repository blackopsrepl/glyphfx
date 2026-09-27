#!/usr/bin/env bash
# Wow clips: a crisp reveal, then a full-screen black hole on the wordmark.
set -u
cd /srv/lab/hack/glyphfx
clear

printf '\n  %scat banner.txt | glyphfx laseretch%s\n\n' $'\033[1m' $'\033[0m'
./build/glyphfx --canvas-width 70 --canvas-height 12 --anchor-canvas c --anchor-text c \
    laseretch < /tmp/opencode/demo/inputs/short.txt
printf '\n\n'

printf '\n  %scat banner.txt | glyphfx blackhole%s\n\n' $'\033[1m' $'\033[0m'
./build/glyphfx --canvas-width 0 --canvas-height 0 --anchor-canvas c --anchor-text c \
    blackhole < /tmp/opencode/demo/inputs/banner.txt
printf '\n'
