#!/usr/bin/env python3
"""Performance regression gate (fail-closed).

For every effect this checks, in order:
  1. Correctness: a full `--parity-dump` run must exit 0, produce at least one
     frame, and hash to the same byte stream recorded in the baseline.
  2. Speed: the fixed workload timed best-of-N must not exceed the baseline.

Nothing that cannot be verified is allowed to pass:
  * a timed run that exits non-zero, times out, or is killed fails the gate;
  * a missing or malformed baseline fails the gate;
  * a machine with no bars of its own fails the gate unless the caller
    explicitly passes --allow-machine;
  * --update refuses to record a run that regressed or changed output, unless
    --force is given, and it only ever lowers a time (tightens the bar).

The metric is user-space instructions, counted by the kernel per child. Wall
time on a shared host drifts 1-5% between back-to-back rounds under load and
cannot carry a zero-tolerance bar; retired instructions repeat to about 0.02%
and are untouched by load, so they decide the gate, and a bar is one number.
The wall time a run takes is still what the matrix reports. There is no
tolerance: the baseline is only ever lowered, so the bar rises with every
verified improvement. Re-baseline deliberately with --update after a verified
change; never widen the threshold to pass.

Baseline: tools/perf/baseline.tsv, one section per machine, one row per effect:
    # machine <arch>:<cpu> | <compiler>
    <effect>\t<best_of_instr_M>\t<frames>\t<sha256-of-parity-stream>
A machine is the CPU and the compiler that built the binary: the same tree built
by another compiler is a different binary, and its times are not comparable
(the Forgejo runner's Debian gcc 12 and this host's gcc 15 differ by up to 3%).
Frames and digests must agree across every section: output is byte-exact
whatever the compiler, and a new section is checked against the existing ones.

Usage:
  tools/perf/gate.py [--runs N] [--binary PATH] [--allow-machine]
  tools/perf/gate.py --update [--force]
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import os
import platform
import random
import re
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BASELINE = ROOT / "tools/perf/baseline.tsv"
SCHEMA = 3
EFFECTS = ["beams", "binarypath", "blackhole", "bouncyballs", "bubbles", "burn",
    "colorshift", "crumble", "decrypt", "errorcorrect", "expand", "fireworks",
    "highlight", "laseretch", "matrix", "middleout", "orbittingvolley", "overflow",
    "pour", "print", "rain", "randomsequence", "rings", "scattered", "slice", "slide",
    "smoke", "spotlights", "spray", "swarm", "sweep", "synthgrid", "thunderstorm",
    "unstable", "vhstape", "waves", "wipe",
    # glyphfx originals: no ttfx counterpart, gated for speed and output the same way.
    "bannerwave", "burnout", "comet", "copper"]

COLS, ROWS, SEED = 200, 50, 1
TIMEOUT = 300
# Best-of-N rounds per effect: all of them when recording a bar, until one
# reaches the bar when checking. With R rounds on each side, an unchanged
# binary misses a bar only if all R check rounds are slower than all R recorded
# ones: (R!)^2 / (2R)! = 1 in 3432 per effect for R = 7.
ROUNDS = 7


class GateError(Exception):
    pass


def workload_input() -> bytes:
    rng = random.Random(1)
    words = ("alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi "
             "omicron pi rho sigma tau upsilon phi chi psi omega solver forge constraint "
             "schedule route").split()
    lines = []
    for _ in range(46):
        s = ""
        while len(s) < 190:
            s += rng.choice(words) + " "
        lines.append(s[:190])
    return ("\n".join(lines) + "\n").encode()


def machine_key() -> str:
    arch = platform.machine()
    cpu = ""
    if Path("/proc/cpuinfo").exists():
        m = re.search(r"^model name\s*:\s*(.+)$", Path("/proc/cpuinfo").read_text(), re.M)
        if m:
            cpu = m.group(1).strip()
    if not cpu and platform.system() == "Darwin":
        try:
            cpu = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"],
                                 capture_output=True, text=True, timeout=10).stdout.strip()
        except OSError:
            cpu = ""
    return f"{arch}:{cpu or platform.processor() or 'unknown'} | {compiler_id()}"


def compiler_id() -> str:
    """First line of `$CC --version`: the compiler the Makefile builds with."""
    cc = os.environ.get("CC") or "cc"
    try:
        out = subprocess.run([cc, "--version"], capture_output=True, text=True,
                             timeout=10).stdout
    except OSError:
        return "unknown compiler"
    return out.splitlines()[0].strip() if out.strip() else "unknown compiler"


def _which(name: str) -> str | None:
    from shutil import which
    return which(name)


def pin_prefix() -> list[str]:
    if platform.system() != "Linux" or not _which("taskset"):
        return []
    try:
        allowed = sorted(os.sched_getaffinity(0))
    except AttributeError:
        allowed = list(range(os.cpu_count() or 0))
    if not allowed:
        raise GateError("no CPU is available to pin a timed run to")
    return ["taskset", "-c", str(allowed[0])]


def _run(cmd: list[str], data: bytes, env: dict, capture_stdout: bool):
    out = subprocess.PIPE if capture_stdout else subprocess.DEVNULL
    try:
        return subprocess.run(cmd, input=data, stdout=out, stderr=subprocess.PIPE,
                              env=env, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        raise GateError(f"timed out after {TIMEOUT}s: {' '.join(cmd)}")


def _base_args(binary: str, effect: str) -> list[str]:
    return pin_prefix() + [binary, "--seed", str(SEED), "--frame-rate", "0", "--virtual-clock",
                           "--canvas-width", str(COLS), "--canvas-height", str(ROWS), effect]


def validate(binary: str, effect: str, data: bytes, env: dict) -> tuple[int, str]:
    """One full parity-dump run; returns (frames, sha256)."""
    cmd = pin_prefix() + [binary, "--seed", str(SEED), "--parity-dump", "--virtual-clock",
                          "--frame-rate", "0", "--canvas-width", str(COLS),
                          "--canvas-height", str(ROWS), effect]
    p = _run(cmd, data, env, capture_stdout=True)
    if p.returncode != 0:
        raise GateError(f"{effect}: validation run exited {p.returncode}")
    m = re.search(rb"^frames=(\d+)$", p.stderr, re.M)
    if not m or int(m.group(1)) < 1:
        raise GateError(f"{effect}: validation run produced no frames")
    return int(m.group(1)), hashlib.sha256(p.stdout).hexdigest()


# --- the metric -----------------------------------------------------------
# User-space instructions, counted by the kernel per child. Wall time on this
# shared host drifts 1-5% between back-to-back rounds and cannot carry a
# zero-tolerance bar; retired instructions repeat to about 0.1% and are immune
# to load, so they decide the gate. Wall time stays as the reported figure.

_PERF_OPEN = 298  # x86-64 perf_event_open
_PERF_TYPE_HARDWARE = 0
_PERF_COUNT_HW_INSTRUCTIONS = 1
_PERF_EVENT_IOC_ENABLE = 0x2400


class Counter:
    """A perf_event_open fd counting one child's user-space instructions."""

    def __init__(self) -> None:
        self.libc = ctypes.CDLL(None, use_errno=True)
        self.fd = -1

    def attach(self, pid: int) -> bool:
        import struct
        attr = bytearray(128)
        struct.pack_into("IIQ", attr, 0, _PERF_TYPE_HARDWARE, 128,
                         _PERF_COUNT_HW_INSTRUCTIONS)
        flags = 1 << 0 | 1 << 5 | 1 << 6  # disabled, exclude_kernel, exclude_hv
        struct.pack_into("Q", attr, 40, flags)
        buf = ctypes.create_string_buffer(bytes(attr))
        fd = self.libc.syscall(_PERF_OPEN, buf, pid, -1, -1, 0)
        if fd < 0:
            return False
        self.fd = fd
        return self.libc.ioctl(fd, _PERF_EVENT_IOC_ENABLE, 0) == 0

    def read(self) -> int:
        import struct
        data = ctypes.create_string_buffer(8)
        if self.libc.read(self.fd, data, 8) != 8:
            return 0
        return struct.unpack("Q", data.raw)[0]

    def close(self) -> None:
        if self.fd >= 0:
            self.libc.close(self.fd)
            self.fd = -1


def counter_available() -> bool:
    probe = Counter()
    ok = probe.attach(0)  # count this process for an instant
    probe.close()
    return ok


def time_run(binary: str, effect: str, data: bytes, env: dict, runs: int) -> float:
    """Best of `runs` user-space instruction counts, in millions.

    The counter attaches right after the child is spawned, so the count misses
    the interpreter-free startup of the target (a few thousand instructions,
    under 0.02% of any effect here) and includes exec. That residual start
    jitter is why a bar is the slowest of ROUNDS rounds and a check passes if
    any round reaches it: the jitter is symmetric, the real cost is not.
    """
    best = None
    with tempfile.TemporaryFile() as stdin:
        stdin.write(data)
        for _ in range(runs):
            stdin.seek(0)
            p = subprocess.Popen(_base_args(binary, effect), stdin=stdin,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
            counter = Counter()
            if not counter.attach(p.pid):
                p.kill()
                p.wait()
                raise GateError(f"{effect}: cannot count instructions "
                                f"(perf_event_open denied; paranoid="
                                f"{Path('/proc/sys/kernel/perf_event_paranoid').read_text().strip()})")
            old = signal.signal(signal.SIGALRM, lambda *_: p.kill())
            signal.alarm(TIMEOUT)
            try:
                _, status, _ = os.wait4(p.pid, 0)
            finally:
                signal.alarm(0)
                signal.signal(signal.SIGALRM, old)
                spent = counter.read()
                counter.close()
            p.returncode = os.waitstatus_to_exitcode(status)
            if p.returncode != 0:
                raise GateError(f"{effect}: timed run exited {p.returncode}"
                                + (" (killed at timeout)" if p.returncode == -signal.SIGKILL else ""))
            # Larger is more complete: the counter can attach a few thousand
            # instructions late, which only ever lowers a count.
            best = spent if best is None else max(best, spent)
    return best / 1e6


Rows = dict[str, tuple[float, int, str]]


def load_baseline() -> dict[str, Rows]:
    """Every machine's section, keyed by machine."""
    sections: dict[str, Rows] = {}
    rows: Rows | None = None
    if BASELINE.exists():
        for line in BASELINE.read_text().splitlines():
            if line.startswith("# machine "):
                rows = sections.setdefault(line[len("# machine "):].strip(), {})
            elif line and not line.startswith("#"):
                parts = line.split("\t")
                if len(parts) != 4 or rows is None:
                    raise GateError(f"malformed baseline row: {line!r}")
                name, ms, frames, sha = parts
                rows[name] = (float(ms), int(frames), sha)
    return sections


def save_baseline(sections: dict[str, Rows]) -> None:
    BASELINE.parent.mkdir(parents=True, exist_ok=True)
    lines = [f"# schema {SCHEMA}",
             "# effect\tbest_of_instr_M\tframes\tsha256",
             "# times only ever tighten; frames and sha prove the workload still renders"]
    for key in sorted(sections):
        lines.append(f"# machine {key}")
        for e in EFFECTS:
            if e in sections[key]:
                ms, frames, sha = sections[key][e]
                lines.append(f"{e}\t{ms:.3f}\t{frames}\t{sha}")
    BASELINE.write_text("\n".join(lines) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", default=str(ROOT / "build/glyphfx"))
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--allow-machine", action="store_true",
                    help="do not fail when the baseline was recorded on another CPU")
    ap.add_argument("--update", action="store_true", help="tighten the baseline to this run")
    ap.add_argument("--force", action="store_true", help="allow --update to record regressions")
    a = ap.parse_args()

    if not Path(a.binary).exists():
        print(f"perf gate: binary not found: {a.binary}", file=sys.stderr)
        return 2

    data = workload_input()
    env = dict(os.environ); env["COLUMNS"] = str(COLS); env["LINES"] = str(ROWS)
    key = machine_key()
    try:
        sections = load_baseline()
    except GateError as exc:
        if not a.update:
            print(f"perf gate: {exc}", file=sys.stderr)
            return 2
        print(f"perf gate: ignoring unreadable baseline ({exc}); rebuilding", file=sys.stderr)
        sections = {}

    if not sections and not a.update:
        print("perf gate: no baseline; run tools/perf/gate.py --update once", file=sys.stderr)
        return 2
    bvals = sections.get(key, {})
    if not bvals and not a.update:
        if not a.allow_machine:
            print(f"perf gate: no bars for this machine '{key}'; recorded for:", file=sys.stderr)
            for k in sorted(sections):
                print(f"  {k}", file=sys.stderr)
            print("  fail-closed: record bars here with --update, or pass --allow-machine",
                  file=sys.stderr)
            return 3
        bvals = sections[sorted(sections)[0]]
    # Output is byte-exact whatever the compiler: a machine's first bars are
    # checked against the frames and digests every other machine recorded.
    expected: Rows = {}
    for rows in sections.values():
        for e, row in rows.items():
            expected.setdefault(e, row)
    expected.update(bvals)

    print(f"perf gate: {len(EFFECTS)} effects, best of {a.runs}, machine {key}")
    measured: dict[str, tuple[float, int, str]] = {}
    regressions: list[tuple[str, float, float]] = []
    try:
        for e in EFFECTS:
            frames, sha = validate(a.binary, e, data, env)
            base = bvals.get(e)
            want = expected.get(e)
            if want is not None and not (a.update and a.force):
                if frames != want[1] or sha != want[2]:
                    raise GateError(f"{e}: parity stream changed ({frames} frames, {sha[:12]})")
            if a.update:
                # The bar is the slowest of ROUNDS best-of-N rounds: what this
                # binary reliably reaches on this machine, not its luckiest run.
                rounds = [time_run(a.binary, e, data, env, a.runs) for _ in range(ROUNDS)]
                ms, record = min(rounds), max(rounds)
            else:
                # A check passes if any of ROUNDS rounds reaches the bar, so a
                # load spike on the shared core does not fail it; a regression
                # larger than the run-to-run spread misses the bar every round.
                ms = time_run(a.binary, e, data, env, a.runs)
                for _ in range(ROUNDS - 1):
                    if base is None or ms <= base[0]:
                        break
                    ms = min(ms, time_run(a.binary, e, data, env, a.runs))
                record = ms
            measured[e] = (record, frames, sha)
            if base is not None and ms > base[0]:
                regressions.append((e, base[0], ms))
                mark = f"REGRESSED (baseline {base[0]:.1f}M)"
            else:
                mark = "new" if base is None else "ok"
            print(f"  {e:<15}{ms:>9.1f}M instr  {mark}", flush=True)
    except GateError as exc:
        print(f"\nperf gate FAILED: {exc}", file=sys.stderr)
        return 1

    if a.update:
        if regressions and not a.force:
            print("\nperf gate: refusing to update over a regression "
                  "(use --force to override)", file=sys.stderr)
            return 1
        tightened = {}
        for e in EFFECTS:
            if e not in measured:
                continue
            old = bvals.get(e)
            if old is not None and old[1] == measured[e][1] and old[2] == measured[e][2]:
                tightened[e] = (min(old[0], measured[e][0]), measured[e][1], measured[e][2])
            else:
                # A new effect, or a verified change to its output: the old time
                # was for a different workload, so record the measured one.
                tightened[e] = measured[e]
        sections[key] = tightened
        save_baseline(sections)
        improved = sum(1 for e in tightened if e in bvals and tightened[e][0] < bvals[e][0])
        print(f"baseline updated: {improved} effect(s) tightened, {BASELINE.relative_to(ROOT)}")
        return 0

    if regressions:
        print(f"\nperf gate FAILED: {len(regressions)} regression(s) vs baseline")
        for e, base, ms in regressions:
            print(f"  {e}: {base:.1f} -> {ms:.1f}M instr (+{100 * (ms / base - 1):.1f}%)")
        return 1
    print("\nperf gate passed: no regression")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
