#!/usr/bin/env bash
# Real-world pipelines: ordinary shell producers piped straight into glyphfx.
set -u
cd /srv/lab/hack/glyphfx
clear

printf '\n  %s1 · git log --oneline -15 | glyphfx matrix%s\n\n' $'\033[1m' $'\033[0m'
git log --oneline -15 | ./build/glyphfx matrix --rain-time 1
printf '\n'

printf '\n  %s2 · ls -la | glyphfx decrypt%s\n\n' $'\033[1m' $'\033[0m'
ls -la | ./build/glyphfx decrypt --typing-speed 12
printf '\n'
