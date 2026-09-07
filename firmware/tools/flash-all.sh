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
  local port="$1" out=""
  stty -f "$port" 115200 raw -echo 2>/dev/null || return 1
  out="$( (head -c 400 "$port" & sleep 6; kill %1 2>/dev/null) 2>/dev/null )"
  case "$out" in
    *"hello tdisplay-s3"*) return 0 ;;
    *) printf 'saw instead: %s\n' "$out" >&2; return 1 ;;
  esac
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

  # A stale build dir would silently reuse the previous unit's UNIT_ID, so
  # force a clean object for the one translation unit that reads it.
  rm -f ".pio/build/${ENV_NAME}/src/main.cpp.o"

  # One image for the whole fleet: UNIT_ID is the only per-board flag.
  build_flags="-DUNIT_ID=${unit}"

  PLATFORMIO_BUILD_FLAGS="$build_flags" \
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
