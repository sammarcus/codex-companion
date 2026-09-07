#!/usr/bin/env bash
# Renders the QR PNG for the printed card (docs/card.md) into docs/out/.
#
# The payload is the card's [URL]: the one place a recipient can fetch the
# single optional helper file from. Keep CARD_URL below in sync with the
# [URL] placeholder on the back of docs/card.md.
#
# It is deliberately NOT a shell command any more. Earlier versions encoded
# an npm/npx install line; there is no package, no tarball and no registry
# involved now, and a QR that pastes a command into someone's terminal is a
# worse idea than a QR that opens a page they can read first.
#
# Nothing is hosted yet, so CARD_URL is a placeholder and this script refuses
# to render until it is replaced. A QR that resolves to nothing is worse than
# no QR at all, and it is the one error you cannot spot on a printed card.
#
# Usage:
#   docs/make-qr.sh              # renders CARD_URL
#   docs/make-qr.sh "some text"  # renders arbitrary text instead

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/out"
OUT_FILE="$OUT_DIR/card-qr.png"

# Replace this with the real URL before printing. It must match the [URL]
# placeholder on the back of docs/card.md.
CARD_URL="REPLACE-ME"

QR_TEXT="${1:-$CARD_URL}"

if [ "$QR_TEXT" = "REPLACE-ME" ]; then
  echo "CARD_URL is still the placeholder."
  echo ""
  echo "Set CARD_URL at the top of this script to the real URL (the same one"
  echo "that replaces [URL] on the back of docs/card.md), or pass the text to"
  echo "encode as an argument:"
  echo ""
  echo "    docs/make-qr.sh \"https://example.com/codex-companion.js\""
  echo ""
  exit 1
fi

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
