#!/usr/bin/env python3
"""Stream a demo protocol sequence at a Codex Desk Companion over USB serial.

For bench-testing a board once hardware is on hand. Nothing here touches the
firmware build.

Usage:
    python3 tools/sim.py                       # auto-pick /dev/cu.usbmodem*
    python3 tools/sim.py --port /dev/cu.usbmodem1101
    python3 tools/sim.py --loop                # repeat the sequence forever

Requires pyserial:  python3 -m pip install --user pyserial
"""

import argparse
import glob
import json
import sys
import time

try:
    import serial  # type: ignore
except ImportError:
    sys.exit("pyserial not installed. Run: python3 -m pip install --user pyserial")

BAUD = 115200


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("no /dev/cu.usbmodem* port found. Plug a board in, or pass --port.")
    return ports[0]


def frames():
    """(delay_seconds, payload_dict) demo sequence covering every state."""
    seq = []

    # idle, context slowly filling
    for pct in range(4, 24, 2):
        seq.append((0.35, {"state": "idle", "ring": pct / 100.0,
                           "center": f"{pct}%", "label": "CTX",
                           "sub": "waiting for work"}))

    # busy, breathing, tokens flowing, elapsed climbing
    for i, pct in enumerate(range(24, 72, 2)):
        mins, secs = divmod(20 + i * 7, 60)
        seq.append((0.30, {"state": "busy", "ring": pct / 100.0,
                           "center": f"{pct}%", "label": "CTX",
                           "sub": f"{mins:02d}:{secs:02d} elapsed",
                           "tps": round(9.0 + (i % 17) * 1.4, 1)}))

    # waiting on a human, held long enough to cross the 10s escalation
    for i in range(46):
        seq.append((0.35, {"state": "waiting", "ring": 0.72,
                           "center": "?", "label": "APPROVE",
                           "sub": "shell: rm -rf build/",
                           "tps": round(4.0 + i * 0.9, 1)}))

    # approved, back to busy, then done
    for pct in range(72, 88, 2):
        seq.append((0.30, {"state": "busy", "ring": pct / 100.0,
                           "center": f"{pct}%", "label": "CTX",
                           "sub": "finishing up", "tps": 22.5}))

    seq.append((2.0, {"state": "done", "ring": 0.88, "center": "OK",
                      "label": "DONE", "sub": "3 files changed", "tps": 0}))

    # quiet: a partial line and a junk line prove the parser ignores them
    seq.append((0.2, {"state": "idle", "ring": 0.88, "center": "88%",
                      "label": "CTX", "sub": "idle"}))
    seq.append((3.0, {}))                      # bare keepalive

    seq.append((2.0, {"state": "sleep", "ring": 0.0, "center": "",
                      "label": "SLEEP", "sub": ""}))
    return seq


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None, help="serial device path")
    ap.add_argument("--loop", action="store_true", help="repeat forever")
    ap.add_argument("--quiet", action="store_true", help="do not echo device replies")
    args = ap.parse_args()

    port = args.port or find_port()
    print(f"opening {port} at {BAUD}", file=sys.stderr)

    with serial.Serial(port, BAUD, timeout=0.05) as ser:
        time.sleep(1.5)                        # let the CDC link settle
        while True:
            for delay, payload in frames():
                line = json.dumps(payload, separators=(",", ":")) + "\n"
                ser.write(line.encode("utf-8"))
                ser.flush()
                time.sleep(delay)
                if not args.quiet:
                    back = ser.read(256)
                    if back:
                        sys.stdout.write(back.decode("utf-8", "replace"))
                        sys.stdout.flush()
            if not args.loop:
                break


if __name__ == "__main__":
    main()
