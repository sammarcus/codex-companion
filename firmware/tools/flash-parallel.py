#!/usr/bin/env python3
"""Flash every attached T-Display-S3 at once, from the one built image.

    cd firmware
    pio run                                  # build once, first
    python3 tools/flash-parallel.py --list   # count the boards
    python3 tools/flash-parallel.py          # flash all of them together

This is the fast path and it is how the first nine units were done. The
one-at-a-time script, tools/flash-all.sh, still works and is the fallback for a
single board that misbehaves.

Every board gets the byte-identical firmware.bin, so there is nothing per unit
to keep straight. Boards are matched on the ESP32-S3 native USB id 303A:1001,
so a USB power meter or a dongle on the same bus is not mistaken for one.

esptool hard-resets each board when it finishes, which plays and SPENDS the
out-of-the-box sequence. tools/arm-all.py puts it back, and it has to be the
last thing that happens to a board before it is boxed. See
docs/wednesday-runbook.md.
"""

import concurrent.futures as cf
import glob
import json
import os
import shlex
import shutil
import subprocess
import sys

HWID = "303A:1001"
HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.join(os.path.dirname(HERE), ".pio", "build", "tdisplays3")
BOOT = os.path.expanduser(
    "~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
)
ESPTOOL_PY = os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py")


def esptool_cmd():
    """First candidate that answers `version` wins.

    The system python3 on this machine has no pyserial, so a bare
    `python3 esptool.py` does not work. Set CODEX_ESPTOOL to override.
    """
    cands = []
    if os.environ.get("CODEX_ESPTOOL"):
        cands.append(shlex.split(os.environ["CODEX_ESPTOOL"]))
    for name in ("esptool", "esptool.py"):
        found = shutil.which(name)
        if found:
            cands.append([found])
    if os.path.exists(ESPTOOL_PY):
        for py in sorted(glob.glob("/opt/homebrew/Cellar/platformio/*/libexec/bin/python")):
            cands.append([py, ESPTOOL_PY])
        cands.append([sys.executable, ESPTOOL_PY])
    for cmd in cands:
        try:
            r = subprocess.run(cmd + ["version"], capture_output=True, text=True, timeout=60)
        except Exception:
            continue
        if r.returncode == 0:
            return cmd
    sys.exit(
        "no working esptool found. Set CODEX_ESPTOOL to one, e.g.\n"
        "  CODEX_ESPTOOL=/path/to/venv/bin/esptool.py python3 tools/flash-parallel.py"
    )


def ports():
    r = subprocess.run(
        ["pio", "device", "list", "--json-output"], capture_output=True, text=True
    )
    if r.returncode != 0:
        sys.exit("pio device list failed: " + (r.stderr or "").strip())
    return sorted(
        d["port"] for d in json.loads(r.stdout) if HWID in (d.get("hwid") or "").upper()
    )


def flash(esp, port):
    cmd = esp + [
        "--chip", "esp32s3", "--port", port, "--baud", "921600",
        "--before", "default_reset", "--after", "hard_reset",
        "write_flash", "-z", "--flash_mode", "dio", "--flash_freq", "80m",
        "--flash_size", "16MB",
        "0x0", os.path.join(FW, "bootloader.bin"),
        "0x8000", os.path.join(FW, "partitions.bin"),
        "0xe000", BOOT,
        "0x10000", os.path.join(FW, "firmware.bin"),
    ]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
    except subprocess.TimeoutExpired:
        return port, False, "timed out after 300s"
    if "Hash of data verified" in r.stdout:
        return port, True, ""
    tail = (r.stdout + r.stderr).strip().splitlines()
    return port, False, tail[-1][:70] if tail else "no output"


def main():
    found = ports()
    if "--list" in sys.argv:
        for p in found:
            print(p)
        print(f"{len(found)} board(s)")
        return 0
    if not found:
        print("0 boards. Check the hub, and use data cables, not charge-only.")
        return 1
    for f in ("bootloader.bin", "partitions.bin", "firmware.bin"):
        if not os.path.exists(os.path.join(FW, f)):
            sys.exit(f"{f} missing. Run `pio run` in firmware/ first.")
    if not os.path.exists(BOOT):
        sys.exit("boot_app0.bin missing from the PlatformIO framework package.")

    esp = esptool_cmd()
    print(f"{len(found)} boards", flush=True)
    bad = 0
    with cf.ThreadPoolExecutor(max_workers=len(found)) as ex:
        for port, ok, err in ex.map(lambda p: flash(esp, p), found):
            if not ok:
                bad += 1
            print(f"{os.path.basename(port):<22} {'OK' if ok else 'FAIL ' + err}", flush=True)
    print("\nEvery board above just booted, which SPENDS its first run.")
    print("Run tools/arm-all.py next, and do not power them again after it.")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
