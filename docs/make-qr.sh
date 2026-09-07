#!/usr/bin/env bash
# Renders the QR PNG for the printed card (docs/card.md) into docs/out/.
#
# By default this encodes the exact one-line setup command from the back
# of the card, not a URL: there is no hosted page or public repo for this
# project yet (confirmed via `git remote -v` at doc-writing time). If a
# public repo or docs page exists by the time cards are printed, change
# QR_TEXT below to that URL instead.
#
# Usage:
#   docs/make-qr.sh              # renders the default setup command
#   docs/make-qr.sh "some text"  # renders arbitrary text instead

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/out"
OUT_FILE="$OUT_DIR/card-qr.png"

# Keep this in sync with docs/card.md's "Set up (one time)" line.
QR_TEXT="${1:-npx codex-companion install}"

if ! command -v qrencode >/dev/null 2>&1; then
  echo "qrencode not found. Install it with:"
  echo ""
  echo "    brew install qrencode"
  echo ""
  echo "then re-run this script."
  exit 1
fi

mkdir -p "$OUT_DIR"

qrencode -o "$OUT_FILE" -s 10 -m 2 -l M "$QR_TEXT"

echo "Wrote $OUT_FILE"
echo "Encoded text: $QR_TEXT"
