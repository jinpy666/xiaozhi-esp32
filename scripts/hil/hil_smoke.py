#!/usr/bin/env python3
"""HIL smoke test: flash firmware on a real device and assert serial-log milestones.

Layer 3 of the testing pyramid (docs/architecture/13-firmware-testing-guide.md).
The core (Milestone / MilestoneRunner) is pure Python with no hardware dependency,
so it is unit-tested offline in scripts/tests/test_hil_smoke.py. Hardware paths
(esptool flashing, pyserial reading) are optional runtime imports; `--from-file`
replays a captured log without any hardware.

Exit codes: 0 all required milestones matched; 1 failed (missing milestone or
panic detected); 2 usage/environment error.

Replay line format (for --from-file): each line may carry an optional timestamp
prefix "[  1234.567] " (seconds since reset); lines without one are spaced 0.1s.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

PANIC_PATTERN = r"Guru Meditation|abort\(\)|assert failed:"

# Generic conservative anchors. Calibrate per product build with --milestones:
# useful anchors are INFO-level state transition logs and the boot banner.
DEFAULT_MILESTONES = [
    {"name": "boot", "pattern": r"\bboot:", "timeout_s": 15, "required": True},
    {"name": "runtime", "pattern": r"(?i)heap|version", "timeout_s": 60, "required": True},
]

_REPLAY_TS = re.compile(r"^\[\s*(\d+(?:\.\d+)?)\]\s?(.*)$")


@dataclass
class Milestone:
    name: str
    pattern: re.Pattern
    timeout_s: float
    required: bool = True
    matched_at: Optional[float] = None

    @classmethod
    def from_dict(cls, d: dict) -> "Milestone":
        return cls(
            name=d["name"],
            pattern=re.compile(d["pattern"]),
            timeout_s=float(d.get("timeout_s", 60)),
            required=bool(d.get("required", True)),
        )

    def to_dict(self) -> dict:
        return {
            "name": self.name,
            "pattern": self.pattern.pattern,
            "timeout_s": self.timeout_s,
            "required": self.required,
            "matched_at": self.matched_at,
        }


class MilestoneRunner:
    """Pure-logic serial-log checker. Timestamps are injected by the caller so
    replay and live modes share one code path.

    Semantics: the run passes when every required milestone has matched and no
    panic was seen. Per-milestone timeout_s auto-expires only *optional*
    milestones (they stop counting as pending); a required milestone left
    unmatched at end of run is a failure. The overall duration is bounded by
    the caller (--timeout)."""

    def __init__(self, milestones: list, panic_pattern: str = PANIC_PATTERN):
        self.milestones: List[Milestone] = [
            m if isinstance(m, Milestone) else Milestone.from_dict(m) for m in milestones
        ]
        self.panic = re.compile(panic_pattern) if panic_pattern else None
        self.panic_line: Optional[str] = None
        self._start: Optional[float] = None

    def mark_start(self, now: float) -> None:
        if self._start is None:
            self._start = now

    def feed_line(self, line: str, now: float) -> None:
        self.mark_start(now)
        line = line.rstrip("\r\n")
        if self.panic and self.panic.search(line) and self.panic_line is None:
            self.panic_line = line
            return
        for m in self.milestones:
            if m.matched_at is None and m.pattern.search(line):
                m.matched_at = now - self._start

    def evaluate(self, now: float) -> None:
        """Apply timeout deadlines; safe to call repeatedly."""
        if self._start is None:
            return
        elapsed = now - self._start
        for m in self.milestones:
            if m.matched_at is None and elapsed > m.timeout_s and not m.required:
                m.matched_at = -1.0  # optional milestone expired: not a failure

    def result(self) -> dict:
        missing = [m for m in self.milestones if m.matched_at is None]
        passed = self.panic_line is None and not missing
        return {
            "passed": passed,
            "panic": self.panic_line,
            "matched": [
                {"name": m.name, "at_s": round(m.matched_at, 3)}
                for m in self.milestones
                if m.matched_at is not None and m.matched_at >= 0
            ],
            "missing": [m.name for m in missing],
            "milestones": [m.to_dict() for m in self.milestones],
        }


def load_milestones(path: Optional[str]) -> list:
    if not path:
        return [dict(m) for m in DEFAULT_MILESTONES]
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(data, list):
        raise ValueError("milestones file must be a JSON array")
    return data


def replay_lines(path: Path) -> "iterable[tuple[str, float]]":
    ts = 0.0
    with path.open("r", encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = _REPLAY_TS.match(raw)
            if m:
                ts = float(m.group(1))
                line = m.group(2)
            else:
                ts += 0.1
                line = raw
            yield line, ts


def run_replay(runner: MilestoneRunner, path: Path) -> dict:
    last = 0.0
    for line, ts in replay_lines(path):
        runner.feed_line(line, ts)
        runner.evaluate(ts)
        if runner.panic_line:
            break
        last = ts
    runner.evaluate(last + 0.5)
    return runner.result()


def flash_firmware(port: str, bin_path: Path) -> None:
    cmd = [sys.executable, "-m", "esptool", "--chip", "auto", "--port", port,
           "--baud", "921600", "--before", "default_reset", "write_flash", "0x0",
           str(bin_path)]
    print("[hil] flashing:", " ".join(cmd), flush=True)
    try:
        subprocess.run(cmd, check=True)
    except FileNotFoundError:
        raise SystemExit("esptool not available: pip install esptool (exit 2)")


def open_serial(port: str, baud: int):
    try:
        import serial  # pyserial: runtime-only dependency
    except ImportError:
        raise SystemExit("pyserial not available: pip install pyserial (exit 2)")
    ser = serial.Serial(port, baud, timeout=1)
    # DTR/RTS toggle for boards wired to EN/BOOT (best effort).
    try:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
    except Exception:
        pass
    return ser


def run_serial(runner: MilestoneRunner, port: str, baud: int, timeout_s: float) -> dict:
    ser = open_serial(port, baud)
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        raw = ser.readline()
        if raw:
            runner.feed_line(raw.decode("utf-8", errors="replace"), time.monotonic())
            print(raw.decode("utf-8", errors="replace"), end="", flush=True)
        runner.evaluate(time.monotonic())
        if runner.panic_line:
            break
        r = runner.result()
        if not r["missing"]:
            break
    ser.close()
    runner.evaluate(time.monotonic() + 0.5)
    return runner.result()


def main(argv: Optional[List[str]] = None) -> int:
    p = argparse.ArgumentParser(description="Firmware HIL smoke test (serial milestones)")
    p.add_argument("--port", help="serial port (required unless --from-file)")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--flash", help="firmware bin to flash first (merged-binary.bin)")
    p.add_argument("--no-flash", action="store_true", help="skip flashing even if --flash given")
    p.add_argument("--from-file", help="replay a captured log instead of a device")
    p.add_argument("--milestones", help="JSON array file overriding default milestones")
    p.add_argument("--timeout", type=float, default=120, help="total serial watch seconds")
    p.add_argument("--json", action="store_true", help="machine-readable result")
    p.add_argument("--list-milestones", action="store_true")
    args = p.parse_args(argv)

    if args.list_milestones:
        print(json.dumps(DEFAULT_MILESTONES, indent=2))
        return 0
    if not args.from_file and not args.port:
        p.error("either --port or --from-file is required")

    runner = MilestoneRunner(load_milestones(args.milestones))

    try:
        if args.from_file:
            result = run_replay(runner, Path(args.from_file))
        else:
            if args.flash and not args.no_flash:
                flash_firmware(args.port, Path(args.flash))
            result = run_serial(runner, args.port, args.baud, args.timeout)
    except SystemExit as e:
        print(str(e), file=sys.stderr)
        return 2

    if args.json:
        print(json.dumps(result, indent=2))
    else:
        for m in result["matched"]:
            print(f"[hil] MATCHED  {m['name']} @ {m['at_s']}s")
        for name in result["missing"]:
            print(f"[hil] MISSING  {name}")
        if result["panic"]:
            print(f"[hil] PANIC    {result['panic']}")
        print(f"[hil] RESULT   {'PASS' if result['passed'] else 'FAIL'}")
    if not result["passed"] and result["panic"]:
        print("[hil] hint: check coredump with `espcoredump.py --port <port> info_corefile`",
              file=sys.stderr)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
