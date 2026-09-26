#!/usr/bin/env python3
"""PTY runtime behavior: SIGTERM teardown is tty-only, SIGINT exits 1 through
normal control flow with teardown, and a closed terminal ends the run quietly."""
import errno
import fcntl
import os
import pty
import signal
import struct
import subprocess
import sys
import termios
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BIN = os.environ.get("GLYPHFX_BIN", os.path.join(ROOT, "build/glyphfx"))

SHOW_CURSOR = b"\x1b[?25h"
INPUT_FILE = "/tmp/glyphfx_sig_input.txt"
with open(INPUT_FILE, "w") as f:
    f.write("Hello\nWorld\n")
# A long-running effect so a signal lands mid-run (real clock, no parity dump).
LONG = ["--seed", "1", "--input-file", INPUT_FILE, "matrix", "--rain-time", "300"]


def child_env():
    env = dict(os.environ)
    # The tty size must come from the pty, not inherited COLUMNS/LINES.
    env.pop("COLUMNS", None)
    env.pop("LINES", None)
    return env


def spawn_pty(args, cols=40, rows=12):
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    proc = subprocess.Popen([BIN] + args, stdin=subprocess.DEVNULL, stdout=slave, stderr=subprocess.DEVNULL,
                            env=child_env())
    os.close(slave)
    return proc, master


def spawn_pipe(args):
    return subprocess.Popen([BIN] + args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, env=child_env())


def drain(fd, seconds):
    os.set_blocking(fd, False)
    end = time.time() + seconds
    out = []
    while time.time() < end:
        try:
            chunk = os.read(fd, 65536)
        except BlockingIOError:
            time.sleep(0.02)
            continue
        except OSError:
            break
        if not chunk:
            break
        out.append(chunk)
    return b"".join(out)


results = []


def check(name, cond, detail=""):
    results.append((name, cond, detail))
    print(f"{'ok  ' if cond else 'FAIL'} {name}{(' — ' + detail) if detail and not cond else ''}")


# 1. SIGTERM on a tty: process dies from the signal, teardown (show cursor) ran.
proc, master = spawn_pty(LONG)
time.sleep(0.3)
proc.send_signal(signal.SIGTERM)
out = drain(master, 0.5)
proc.wait()
try:
    os.close(master)
except OSError:
    pass
check("SIGTERM tty dies from signal", proc.returncode == -signal.SIGTERM, f"rc={proc.returncode}")
check("SIGTERM tty runs teardown", SHOW_CURSOR in out, f"bytes={len(out)}")

# 2. SIGTERM redirected: no teardown bytes may be added.
proc = spawn_pipe(LONG)
time.sleep(0.3)
proc.send_signal(signal.SIGTERM)
out, _ = proc.communicate(timeout=5)
check("SIGTERM redirected dies from signal", proc.returncode == -signal.SIGTERM, f"rc={proc.returncode}")
check("SIGTERM redirected adds no teardown", SHOW_CURSOR not in out, f"bytes={len(out)}")

# 3. SIGINT on a tty: normal control flow, teardown, exit 1.
proc, master = spawn_pty(LONG)
time.sleep(0.3)
proc.send_signal(signal.SIGINT)
out = drain(master, 0.5)
proc.wait()
try:
    os.close(master)
except OSError:
    pass
check("SIGINT tty exits 1", proc.returncode == 1, f"rc={proc.returncode}")
check("SIGINT tty runs teardown", SHOW_CURSOR in out, f"bytes={len(out)}")

# 4. Terminal close: closing the master ends the run without a crash.
proc, master = spawn_pty(LONG)
time.sleep(0.3)
os.close(master)
try:
    proc.wait(timeout=5)
except subprocess.TimeoutExpired:
    proc.kill()
    proc.wait()
check("closed terminal ends the run", proc.returncode == 0, f"rc={proc.returncode}")

# 5. Resize: SIGWINCH with a changed window size restarts the run in place.
# Canvas sized to the terminal so a resize actually moves cells.
LONG0 = ["--seed", "1", "--input-file", INPUT_FILE, "--canvas-width", "0", "--canvas-height", "0",
         "matrix", "--rain-time", "300"]
proc, master = spawn_pty(LONG0, cols=40, rows=12)
time.sleep(0.3)
drain(master, 0.1)
fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 16, 52, 0, 0))
proc.send_signal(signal.SIGWINCH)
time.sleep(0.3)
out = drain(master, 0.3)
check("resize keeps the run alive", proc.poll() is None, f"rc={proc.poll()}")
check("resize wipes the old area", b"\x1b[0J" in out, f"bytes={len(out)}")
proc.send_signal(signal.SIGINT)
drain(master, 0.3)
proc.wait()
try:
    os.close(master)
except OSError:
    pass

failed = [r for r in results if not r[1]]
print(f"tty signals: {len(results) - len(failed)} passed, {len(failed)} failed")
sys.exit(1 if failed else 0)
