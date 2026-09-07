#!/usr/bin/env bash
# Flash all 14 Codex Desk Companion units, stamping each with its UNIT_ID.
#
# No names are needed here, and none can be given here. Every board flashes
# from one identical image; the only thing that varies between them is
# -DUNIT_ID=<n>. Owner names are set afterwards, at runtime, by sending a
# protocol line carrying "name" over USB serial, which the device persists to
# NVS. See ../README.md.
#
# Run from the firmware/ directory. For each unit it waits for a board to
# enumerate with the ESP32-S3 native USB id, builds with -DUNIT_ID=<n>,
# uploads, verifies the serial greeting, then prompts you to swap in the next
# board.
#
#   ./tools/flash-all.sh          # units 1..14
#   ./tools/flash-all.sh 5 8      # units 5..8 only
#
# Nothing here runs during a build; it is operator tooling for assembly day.

set -euo pipefail

FIRST="${1:-1}"
LAST="${2:-14}"
ENV_NAME="tdisplays3"
PORT_GLOB="/dev/cu.usbmodem*"
WAIT_TIMEOUT=120

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$here"

command -v pio >/dev/null 2>&1 || { echo "pio not on PATH" >&2; exit 1; }

# The board is the only thing on the bus with the ESP32-S3 native USB id.
# Matching the first /dev/cu.usbmodem* is not safe: a USB power meter, a
# Seeed dongle or a second board all enumerate under that same glob.
BOARD_HWID="303A:1001"

find_port() {
  pio device list --json-output 2>/dev/null | python3 -c "
import json,sys
want = '$BOARD_HWID'.upper()
ports = [d['port'] for d in json.load(sys.stdin)
         if want in (d.get('hwid') or '').upper()]
print(ports[0] if ports else '')
"
}

port_serial() {
  pio device list --json-output 2>/dev/null | python3 -c "
import json,re,sys
for d in json.load(sys.stdin):
    if d['port'] == '$1':
        m = re.search(r'SER=(\\S+)', d.get('hwid') or '')
        print(m.group(1) if m else 'unknown')
        break
"
}

# esptool hard-resets the board when it is done, so the boot banner is on the
# wire immediately after upload. Reading it proves the flash actually runs.
verify_hello() {
  local port="$1"
  # Self-contained: resets the board itself, then reads the greeting. Three
  # things this has to work around, all found on real hardware:
  #  1. `head -c N` blocks until N bytes arrive and the greeting is short, and
  #     `kill %1` inside a command substitution is unreliable without job
  #     control, so the old version could hang. This uses a hard deadline.
  #  2. Opening the port after esptool has already reset the board misses the
  #     greeting entirely, so we pulse DTR and RTS and read our own boot.
  #  3. The device's USB CDC transmit runs one message behind: the greeting only
  #     leaves the chip once another line is written. We nudge with `{}`, which
  #     the firmware accepts and acks without changing any state.
  # On success it echoes the greeting line, which carries the stored owner name
  # and is the only external proof of what is actually on the unit.
  python3 - "$port" <<'PY'
import os, sys, time, struct, fcntl, termios

port, want = sys.argv[1], "hello tdisplay-s3"
try:
    fd = os.open(port, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
except OSError as e:
    print("could not open %s: %s" % (port, e), file=sys.stderr)
    sys.exit(1)

def modem(op, mask):
    fcntl.ioctl(fd, op, struct.pack("I", mask))

try:
    modem(termios.TIOCMBIC, termios.TIOCM_DTR)
    modem(termios.TIOCMBIS, termios.TIOCM_RTS)
    time.sleep(0.15)
    modem(termios.TIOCMBIS, termios.TIOCM_DTR)
    modem(termios.TIOCMBIC, termios.TIOCM_RTS)

    deadline = time.time() + 12.0
    seen = ""
    last_nudge = 0.0
    while time.time() < deadline:
        now = time.time()
        if now - last_nudge > 1.0 and want not in seen:
            try:
                os.write(fd, b"{}\n")
            except OSError:
                pass
            last_nudge = now
        try:
            chunk = os.read(fd, 512)
        except BlockingIOError:
            time.sleep(0.05)
            continue
        except OSError as e:
            print("read error: %s" % e, file=sys.stderr)
            sys.exit(1)
        if chunk:
            seen += chunk.decode("utf-8", "replace")
            if want in seen:
                for line in seen.splitlines():
                    if want in line:
                        print(line.strip())
                        sys.exit(0)
        else:
            time.sleep(0.05)

    print("no greeting within 12s, last bytes: %r" % seen[-200:], file=sys.stderr)
    sys.exit(1)
finally:
    os.close(fd)
PY
}

wait_for_port() {
  local waited=0 port=""
  while [ "$waited" -lt "$WAIT_TIMEOUT" ]; do
    port="$(find_port || true)"
    if [ -n "$port" ]; then
      echo "$port"
      return 0
    fi
    sleep 1
    waited=$((waited + 1))
  done
  return 1
}

wait_for_no_port() {
  local waited=0
  while [ "$waited" -lt "$WAIT_TIMEOUT" ]; do
    if [ -z "$(find_port || true)" ]; then
      return 0
    fi
    sleep 1
    waited=$((waited + 1))
  done
  return 1
}

for unit in $(seq "$FIRST" "$LAST"); do
  echo
  echo "=============================================="
  echo " UNIT $unit of $LAST"
  echo "=============================================="
  echo "Plug in the board for unit $unit."

  if ! port="$(wait_for_port)"; then
    echo "no board appeared within ${WAIT_TIMEOUT}s. Aborting at unit $unit." >&2
    exit 1
  fi
  echo "found $port"

  # Every unit gets the byte-identical image. Nothing is compiled per board,
  # so there is one binary and one hash for the whole fleet, which is what
  # makes "here is the source, here is the hash, dump your own board and
  # compare" an honest claim rather than a fourteen-way asterisk.
  #
  # Units still identify themselves: with no -DUNIT_ID the firmware derives a
  # label from the chip's own MAC, so each board shows a distinct id without
  # anything being stamped in at build time. The counter below is only the
  # operator's place in the run, for the fleet log.
  #
  # -DUNIT_ID is still honoured if someone wants numbered units; set
  # UNIT_BUILD_FLAGS to pass it, and accept that each unit then has its own
  # binary and its own hash.
  PLATFORMIO_BUILD_FLAGS="${UNIT_BUILD_FLAGS:-}" \
    pio run -e "$ENV_NAME" -t upload --upload-port "$port"

  serial="$(port_serial "$port")"
  if verify_hello "$port"; then
    echo "unit $unit verified: board greeted over serial"
    printf '%s\t%s\t%s\t%s\n' "$unit" "$serial" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "ok" >> tools/fleet-log.tsv
  else
    echo "unit $unit FLASHED BUT DID NOT GREET. Set it aside." >&2
    printf '%s\t%s\t%s\t%s\n' "$unit" "$serial" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "no-greeting" >> tools/fleet-log.tsv
  fi

  echo "unit $unit flashed. Unplug it."
  if ! wait_for_no_port; then
    echo "board still present after ${WAIT_TIMEOUT}s; continuing anyway." >&2
  fi
done

echo
echo "All units $FIRST..$LAST flashed."
