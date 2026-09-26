#!/usr/bin/env bash
# CLI corpus: exit codes and stdout/stderr routing versus the ttfx oracle.
# Message text may differ; conditions, exit codes, and which stream carries the
# message must match.
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ORACLE="${GLYPHFX_TTFX:-$ROOT/reference/target/release/ttfx}"
SUBJECT="${GLYPHFX_BIN:-$ROOT/build/glyphfx}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

[ -x "$ORACLE" ] || { echo "oracle not found: $ORACLE" >&2; exit 2; }
[ -x "$SUBJECT" ] || { echo "subject not found: $SUBJECT (run make)" >&2; exit 2; }

pass=0
fail=0

# classify a stream: empty / nonempty
classify() { if [ -s "$1" ]; then printf 'some'; else printf 'none'; fi; }

# run_case <description> <stdin-file-or--> <args...>
run_case() {
    local desc="$1"; shift
    local stdin_file="$1"; shift
    "$ORACLE" "$@" < "$stdin_file" > "$TMP/oracle.out" 2> "$TMP/oracle.err"
    local orc=$?
    "$SUBJECT" "$@" < "$stdin_file" > "$TMP/subject.out" 2> "$TMP/subject.err"
    local src=$?
    local ok=1
    [ "$orc" -eq "$src" ] || ok=0
    # stream routing must match: stdout-empty vs stdout-some, same for stderr
    [ "$(classify "$TMP/oracle.out")" = "$(classify "$TMP/subject.out")" ] || ok=0
    [ "$(classify "$TMP/oracle.err")" = "$(classify "$TMP/subject.err")" ] || ok=0
    if [ "$ok" -eq 1 ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL $desc (oracle rc=$orc out=$(classify "$TMP/oracle.out") err=$(classify "$TMP/oracle.err") | subject rc=$src out=$(classify "$TMP/subject.out") err=$(classify "$TMP/subject.err"))"
    fi
}

printf 'Hello\n' > "$TMP/in.txt"
printf '' > "$TMP/empty.txt"
printf '   \n' > "$TMP/spaces.txt"
printf 'ok' > "$TMP/ok.txt"
printf 'abc\x1b[999xdef\n' > "$TMP/bad_ansi.txt"
printf 'abc\xff\xfe\n' > "$TMP/bad_utf8.txt"

run_case "no input (empty stdin)" "$TMP/empty.txt"
run_case "whitespace-only stdin" "$TMP/spaces.txt"
run_case "no effect" "$TMP/in.txt"
run_case "unknown effect" "$TMP/in.txt" not-an-effect
run_case "unknown option" "$TMP/in.txt" --definitely-not-an-option
run_case "missing input file" "$TMP/in.txt" --input-file "$TMP/does-not-exist"
run_case "unsupported ANSI" "$TMP/bad_ansi.txt" --seed 1 --parity-dump --max-frames 1 --virtual-clock wipe
run_case "invalid utf8 stdin" "$TMP/bad_utf8.txt" --seed 1 --parity-dump --max-frames 1 --virtual-clock wipe
run_case "version" "$TMP/in.txt" --version
run_case "random-effect empty filter" "$TMP/ok.txt" --random-effect --include-effects no-such-effect
run_case "include and exclude conflict" "$TMP/ok.txt" --random-effect --include-effects beams --exclude-effects wipe
run_case "bad effect option value" "$TMP/in.txt" wipe --wipe-delay -3
run_case "bad terminal option value" "$TMP/in.txt" --frame-rate -1 wipe
run_case "effect option without value" "$TMP/in.txt" wipe --wipe-ease

echo "cli corpus: $pass passed, $fail failed"
[ "$fail" -eq 0 ] || exit 1
