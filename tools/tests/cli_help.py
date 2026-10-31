#!/usr/bin/env python3
"""glyphfx's own help: when it appears, how it is drawn, and that it is current.

  - At a terminal with no input, glyphfx prints its help (an effect named
    alone prints that effect's options). A pipe, even an empty one, still
    gets ttfx's "NO INPUT." (tools/tests/cli_corpus.sh pins that).
  - On a colour terminal --help opens with the Makefile's wordmark, drawn by
    the highlight effect; NO_COLOR, TERM=dumb, --no-color, a pipe and a
    terminal too small for the wordmark all get plain text, the same words.
  - Every line fits the terminal it was drawn for.
  - src/cli_help.c matches what tools/gen_cli_help.py generates (skipped when
    the reference ttfx is not built).
"""
from __future__ import annotations

import fcntl
import os
import pty
import re
import select
import struct
import subprocess
import sys
import termios
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BIN = os.environ.get("GLYPHFX_BIN", str(ROOT / "build/glyphfx"))
ESC = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]|\x1b[78]")
LOGO_TOP = " ██████╗ ██╗  ██╗   ██╗██████╗ ██╗  ██╗███████╗██╗  ██╗"
EMERALD = b"38;2;16;185;129"

fails: list[str] = []
checks = 0


def check(name: str, cond: bool, detail: str = "") -> None:
    global checks
    checks += 1
    if not cond:
        fails.append(f"{name}{': ' + detail if detail else ''}")


def env(**extra: str) -> dict[str, str]:
    e = {k: v for k, v in os.environ.items() if k not in ("NO_COLOR", "COLUMNS", "LINES")}
    e["TERM"] = "xterm-256color"
    e.update(extra)
    return e


def run_pty(args: list[str], cols: int = 120, rows: int = 40, stdin_tty: bool = True,
            stdin_data: bytes | None = None, extra_env: dict[str, str] | None = None) -> tuple[int, bytes]:
    """Run with stdout (and, by default, stdin) on a pseudo-terminal."""
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    stdin = slave if stdin_tty else (subprocess.PIPE if stdin_data is not None else subprocess.DEVNULL)
    p = subprocess.Popen([BIN, *args], stdin=stdin, stdout=slave, stderr=slave,
                         env=env(**(extra_env or {})), close_fds=True)
    os.close(slave)
    if stdin_data is not None:
        p.stdin.write(stdin_data)
        p.stdin.close()
    out = b""
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        r, _, _ = select.select([master], [], [], 0.05)
        if r:
            try:
                chunk = os.read(master, 65536)
            except OSError:
                break
            if not chunk:
                break
            out += chunk
        elif p.poll() is not None:
            break
    else:
        p.kill()
    rc = p.wait()
    os.close(master)
    return rc, out


def plain(out: bytes) -> str:
    return ESC.sub(b"", out).decode("utf-8", "replace").replace("\r", "")


def body(text: str) -> str:
    """The help from its Usage line on: the part every style shares."""
    i = text.find("Usage:")
    return text[i:] if i >= 0 else ""


def main() -> int:
    piped = subprocess.run([BIN, "--help"], capture_output=True, timeout=30)
    piped_text = piped.stdout.decode()
    check("--help piped exits 0", piped.returncode == 0, str(piped.returncode))
    check("--help piped has no escapes", b"\x1b" not in piped.stdout)
    check("--help piped has no wordmark", LOGO_TOP not in piped_text)
    check("--help piped describes options", "--frame-rate <FPS>" in piped_text
          and "Target frames per second" in piped_text and "[default: 60]" in piped_text)
    check("--help piped lists the originals", "glyphfx originals:" in piped_text and "copper" in piped_text)
    check("--help piped keeps one entry per line", all(not ln.startswith(" " * 18) for ln in piped_text.split("\n")))

    # No input at a terminal: the help, animated.
    rc, out = run_pty([])
    text = plain(out)
    check("no input at a tty exits 0", rc == 0, str(rc))
    check("no input at a tty draws the wordmark", LOGO_TOP in text)
    check("no input at a tty animates in emerald", EMERALD in out or b"38;5;" in out)
    check("no input at a tty prints the help", body(text).startswith("Usage:") and "Effects:" in text)
    check("no input at a tty shows the version", "v" + piped_text.split()[1] in text, piped_text.split("\n", 1)[0])
    check("wordmark comes before the help", 0 <= text.find(LOGO_TOP) < text.find("Usage:"))
    check("tty help says the same as piped help", body(text).split() == body(piped_text).split())
    longest = max(len(ln) for ln in body(text).split("\n"))
    check("tty help fits 100 columns", longest <= 100, f"widest line {longest}")

    # An effect named alone: that effect's options.
    rc, out = run_pty(["beams"])
    text = plain(out)
    check("effect with no input exits 0", rc == 0, str(rc))
    check("effect with no input prints its options",
          "Usage: glyphfx beams [OPTIONS]" in text and "--beam-delay <BEAM_DELAY>" in text)
    check("effect help has no wordmark", LOGO_TOP not in text)

    # Plain at a terminal that does not want colour, or is too small.
    for name, args, extra, cols in (("NO_COLOR", [], {"NO_COLOR": "1"}, 120),
                                    ("TERM=dumb", [], {"TERM": "dumb"}, 120),
                                    ("--no-color", ["--no-color"], {}, 120),
                                    ("narrow", [], {}, 50)):
        rc, out = run_pty(args, cols=cols, extra_env=extra)
        text = plain(out)
        check(f"{name}: exits 0", rc == 0, str(rc))
        check(f"{name}: no wordmark", LOGO_TOP not in text)
        if name != "narrow":
            check(f"{name}: no escapes", b"\x1b" not in out)
        check(f"{name}: same words", body(text).split() == body(piped_text).split())
        longest = max(len(ln) for ln in text.split("\n"))
        check(f"{name}: fits {min(cols, 100)} columns", longest <= min(cols, 100), f"widest line {longest}")

    # A pipe never turns into help: ttfx's "NO INPUT." stays, on stdout.
    for name, data in (("empty pipe", b""), ("blank pipe", b"\n")):
        rc, out = run_pty(["beams"], stdin_tty=False, stdin_data=data)
        check(f"{name}: NO INPUT.", "NO INPUT." in plain(out) and "Usage:" not in plain(out), plain(out)[:80])
    rc, out = run_pty([], stdin_tty=False, stdin_data=b"")
    check("pipe without an effect is not help", "Usage:" not in plain(out) or "error" in plain(out).lower())

    # Ctrl-C during the wordmark ends the run without the help, like any effect.
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
    p = subprocess.Popen([BIN, "--help"], stdin=slave, stdout=slave, stderr=slave, env=env(), close_fds=True)
    os.close(slave)
    time.sleep(0.3)
    p.send_signal(2)
    out = b""
    while True:
        r, _, _ = select.select([master], [], [], 0.2)
        if not r:
            if p.poll() is not None:
                break
            continue
        try:
            chunk = os.read(master, 65536)
        except OSError:
            break
        if not chunk:
            break
        out += chunk
    rc = p.wait(timeout=10)
    os.close(master)
    check("ctrl-c during the wordmark exits 1", rc == 1, str(rc))
    check("ctrl-c during the wordmark skips the help", "Effects:" not in plain(out))
    check("ctrl-c during the wordmark restores the cursor", out.rstrip().endswith(b"\x1b[?25h")
          or b"\x1b[?25h" in out[-64:], repr(out[-32:]))

    # The committed table is what the generator writes.
    ttfx = Path(os.environ.get("GLYPHFX_TTFX", ROOT / "reference/target/release/ttfx"))
    if ttfx.exists():
        g = subprocess.run([sys.executable, str(ROOT / "tools/gen_cli_help.py"), "--check"],
                           capture_output=True, text=True, timeout=120)
        check("src/cli_help.c is current", g.returncode == 0, g.stdout.strip())
    else:
        print(f"cli_help: {ttfx} not built; skipping the src/cli_help.c freshness check")

    for f in fails:
        print(f"FAIL {f}")
    print(f"cli_help: {checks - len(fails)}/{checks} checks passed")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
