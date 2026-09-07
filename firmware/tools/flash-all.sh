#!/usr/bin/env bash
# Flash all 14 Codex Desk Companion units, stamping each with its UNIT_ID.
#
# Run from the firmware/ directory. For each unit it waits for a board to
# enumerate as /dev/cu.usbmodem*, builds with -DUNIT_ID=<n>, uploads, then
# prompts you to swap in the next board.
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

find_port() {
  # shellcheck disable=SC2086
  ls -1 $PORT_GLOB 2>/dev/null | head -1
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

  PLATFORMIO_BUILD_FLAGS="-DUNIT_ID=${unit}" \
    pio run -e "$ENV_NAME" -t upload --upload-port "$port"

  echo "unit $unit flashed. Unplug it."
  if ! wait_for_no_port; then
    echo "board still present after ${WAIT_TIMEOUT}s; continuing anyway." >&2
  fi
done

echo
echo "All units $FIRST..$LAST flashed."
