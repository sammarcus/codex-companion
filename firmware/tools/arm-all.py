#!/usr/bin/env python3
"""Factory reset every attached board, in parallel. The last step before boxing.

    cd firmware
    python3 tools/arm-all.py

A factory reset wipes the whole stored namespace (owner name, face, best score,
stats records, focus length) and re-arms the out-of-the-box sequence, so the
recipient gets the first run. That is the point: anything that powers a board
plays and spends that sequence, and flashing, verifying and demoing all power
it. This has to be the LAST thing done to a board, and nothing may plug it in
afterwards.

If a board is NAMED, do not use this: a factory reset takes the name with it.
Send {"reset":"firstrun"} to that board instead, which re-arms and keeps the
name and the face.

Expect ARMED on every line. Anything else, see docs/wednesday-runbook.md.
"""

import concurrent.futures as cf
import json
import os
import subprocess
import sys
import time

HWID = "303A:1001"


def ports():
    r = subprocess.run(
        ["pio", "device", "list", "--json-output"], capture_output=True, text=True
    )
    if r.returncode != 0:
        sys.exit("pio device list failed: " + (r.stderr or "").strip())
    return sorted(
        d["port"] for d in json.loads(r.stdout) if HWID in (d.get("hwid") or "").upper()
    )


def arm(port):
    """Write the reset, then read the reply.

    The board's USB CDC transmit runs one message behind, so the reply only
    leaves the chip once something else is written. `{}` is the protocol's
    no-op, so the nudge cannot change any state on the board it is checking.
    The board is deliberately NOT reset here: a reset would play the sequence
    again and spend the flag just restored.
    """
    try:
        fd = os.open(port, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
    except OSError as e:
        return port, False, f"could not open: {e}"
    try:
        os.write(fd, b'{"reset":"factory"}\n')
        time.sleep(0.4)
        os.write(fd, b"{}\n")
        seen, end = "", time.time() + 6
        while time.time() < end:
            try:
                chunk = os.read(fd, 512)
            except BlockingIOError:
                time.sleep(0.05)
                continue
            except OSError as e:
                return port, False, f"read error: {e}"
            if chunk:
                seen += chunk.decode("utf-8", "replace")
                # "ram-only" means NVS refused the write: armed for this power
                # cycle only, and it dies with the unplug that comes next.
                if "reset: factory ok" in seen:
                    return port, True, ""
                if "reset: factory ram-only" in seen:
                    return port, False, "ram-only, NVS refused. NOT shippable"
            else:
                time.sleep(0.05)
        return port, False, "no reply in 6s, last bytes: " + repr(seen[-60:])
    finally:
        os.close(fd)


def main():
    found = ports()
    if not found:
        print("0 boards.")
        return 1
    bad = 0
    with cf.ThreadPoolExecutor(max_workers=len(found)) as ex:
        for port, ok, err in ex.map(arm, found):
            if not ok:
                bad += 1
            print(f"{os.path.basename(port):<22} {'ARMED' if ok else 'NOT ARMED: ' + err}",
                  flush=True)
    print(f"\n{len(found) - bad} of {len(found)} armed.")
    print("Unplug them now. Anything that powers a board again spends its first run.")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
