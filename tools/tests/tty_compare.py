#!/usr/bin/env python3
"""PTY byte-stream parity: run both binaries on a pty and compare the entire
prep + frames + teardown stream, including the cursor/teardown variants."""
import fcntl
import os
import pty
import struct
import subprocess
import sys
import termios

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ORACLE = os.environ.get("GLYPHFX_TTFX", os.path.join(ROOT, "reference/target/release/ttfx"))
SUBJECT = os.environ.get("GLYPHFX_BIN", os.path.join(ROOT, "build/glyphfx"))

COLS, ROWS = 30, 10


def run(binary, args, data):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
    proc = subprocess.Popen([binary] + args, stdin=subprocess.PIPE, stdout=slave, stderr=subprocess.DEVNULL)
    os.close(slave)
    proc.stdin.write(data)
    proc.stdin.close()
    chunks = []
    while True:
        try:
            chunk = os.read(master, 65536)
        except OSError:
            break
        if not chunk:
            break
        chunks.append(chunk)
    os.close(master)
    proc.wait()
    return b"".join(chunks), proc.returncode


def main():
    if not os.path.exists(ORACLE) or not os.path.exists(SUBJECT):
        print("oracle or subject missing", file=sys.stderr)
        return 2
    data = b"Hello\nWorld\n"
    variants = [
        [],
        ["--reuse-canvas"],
        ["--no-eol"],
        ["--no-restore-cursor"],
        ["--reuse-canvas", "--no-eol", "--no-restore-cursor"],
    ]
    effects = ["randomsequence", "wipe", "decrypt"]
    passed = 0
    failed = 0
    for effect in effects:
        for variant in variants:
            args = ["--seed", "42", "--frame-rate", "0", "--virtual-clock"] + variant + [effect]
            oracle_out, orc = run(ORACLE, args, data)
            subject_out, src = run(SUBJECT, args, data)
            if orc != src:
                failed += 1
                print(f"FAIL {effect} {' '.join(variant)}: exit {orc} != {src}")
                continue
            if oracle_out != subject_out:
                failed += 1
                n = min(len(oracle_out), len(subject_out))
                diff = next((i for i in range(n) if oracle_out[i] != subject_out[i]), n)
                print(f"FAIL {effect} {' '.join(variant)}: byte {diff} "
                      f"(oracle {len(oracle_out)}B, subject {len(subject_out)}B)")
                continue
            passed += 1
    print(f"tty compare: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
