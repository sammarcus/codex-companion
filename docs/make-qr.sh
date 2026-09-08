#!/usr/bin/env bash
# Renders the QR for the printed card (docs/card.md) into docs/out/.
#
# The payload is the card's [URL]: the one place a recipient can fetch the
# single optional helper file from.
#
# It is deliberately NOT a shell command. Earlier versions encoded an npm/npx
# install line; there is no package, no tarball and no registry involved now,
# and a QR that pastes a command into someone's terminal is a worse idea than
# a QR that opens a page they can read first.
#
# Where the URL comes from, first match wins:
#
#   1. the argument:      docs/make-qr.sh "https://example.com/x.js"
#   2. the environment:   CARD_URL=https://... docs/make-qr.sh
#   3. docs/card-url.txt  one line, the URL, created by you, not in git
#   4. CARD_URL below     still the placeholder, so the script refuses
#
# Nothing is hosted yet (docs/open-questions.md, N1), so the built-in default
# is a placeholder and this script refuses to render from it. A QR that
# resolves to nothing is worse than no QR, and it is the one error you cannot
# spot on a printed card. Options 1 to 3 exist so that filling in the real URL
# on print day does not mean editing a shell script.
#
# It renders TWICE and checks its own work:
#
#   docs/out/card-qr.png   opaque white background, 300 dpi, for a layout tool
#   docs/out/card-qr.svg   vector, for a printer that wants it
#
# then decodes the PNG back and compares it to what it meant to encode. If no
# decoder is installed it says the render was NOT verified rather than
# claiming it was.
#
# Usage:
#   docs/make-qr.sh                     # URL from env, or card-url.txt
#   docs/make-qr.sh "https://..."       # URL on the command line
#   docs/make-qr.sh --print-only "..."  # skip the decode-back check

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT_DIR="$SCRIPT_DIR/out"
OUT_PNG="$OUT_DIR/card-qr.png"
OUT_SVG="$OUT_DIR/card-qr.svg"
URL_FILE="$SCRIPT_DIR/card-url.txt"

# Whatever the environment supplied, captured BEFORE the default below clobbers
# it. Without this line `CARD_URL=... docs/make-qr.sh` would silently do
# nothing, which is the worst failure available to a script whose whole job is
# to refuse to encode the wrong string.
CARD_URL_ENV="${CARD_URL:-}"

# Replace this, or better, use one of the three ways above that do not need
# the file edited. It must match the [URL] placeholder on docs/card.md.
CARD_URL="REPLACE-ME"

# Print geometry. 10 dots per module at 300 dpi puts a version 3 symbol at
# about 31mm square, which is a comfortable size on a business card and well
# above the roughly 20mm floor for a phone camera at arm's length.
MODULE_DOTS=10
DPI=300
# Margin 4 is the quiet zone the QR spec requires. An earlier version of this
# script used 2, which scans fine on a screen and is the classic reason a
# printed code fails against a busy background.
MARGIN=4
ECC=M

VERIFY=1
if [ "${1:-}" = "--print-only" ]; then
  VERIFY=0
  shift
fi

url_from_file() {
  [ -f "$URL_FILE" ] || return 1
  # first line that is neither blank nor a comment
  awk 'NF && $1 !~ /^#/ { print $0; exit }' "$URL_FILE"
}

QR_TEXT="${1:-}"
SOURCE="the command line"

if [ -z "$QR_TEXT" ] && [ -n "$CARD_URL_ENV" ]; then
  QR_TEXT="$CARD_URL_ENV"
  SOURCE="the CARD_URL environment variable"
fi

if [ -z "$QR_TEXT" ]; then
  if from_file="$(url_from_file)" && [ -n "$from_file" ]; then
    QR_TEXT="$from_file"
    SOURCE="docs/card-url.txt"
  fi
fi

if [ -z "$QR_TEXT" ]; then
  QR_TEXT="$CARD_URL"
  SOURCE="the CARD_URL default in this script"
fi

if [ "$QR_TEXT" = "REPLACE-ME" ]; then
  cat >&2 <<'MSG'
No URL to encode: CARD_URL is still the placeholder.

Nothing is hosted yet, so this script will not render a QR that resolves to
nothing. Give it the real URL in any one of these ways, easiest first:

    docs/make-qr.sh "https://example.com/codex-companion.js"

    echo "https://example.com/codex-companion.js" > docs/card-url.txt
    docs/make-qr.sh

    CARD_URL=https://example.com/codex-companion.js docs/make-qr.sh

Whatever you use, put the same string in place of [URL] on the back of
docs/card.md before printing. See docs/open-questions.md, item N1.
MSG
  exit 1
fi

if ! command -v qrencode >/dev/null 2>&1; then
  cat >&2 <<'MSG'
qrencode not found. Install it with:

    brew install qrencode

then re-run this script. The QR is the first thing on the cut list
(docs/wednesday-runbook.md section 8), so a card without one is fine.
MSG
  exit 1
fi

mkdir -p "$OUT_DIR"

# PNG32 with an explicitly opaque white background. qrencode's default PNG is
# palettised with a transparent background, which lands as black on black the
# first time somebody drops it into a dark layout and is invisible until it is
# printed.
qrencode -o "$OUT_PNG" -t PNG32 -s "$MODULE_DOTS" -m "$MARGIN" -l "$ECC" -d "$DPI" \
  --foreground=000000 --background=FFFFFF -- "$QR_TEXT"

qrencode -o "$OUT_SVG" -t SVG -s "$MODULE_DOTS" -m "$MARGIN" -l "$ECC" \
  --foreground=000000 --background=FFFFFF -- "$QR_TEXT"

echo "Wrote $OUT_PNG"
echo "Wrote $OUT_SVG"
echo "Encoded: $QR_TEXT"
echo "URL came from: $SOURCE"

python3 - "$OUT_PNG" "$DPI" <<'PY'
import sys
try:
    from PIL import Image
except ImportError:
    sys.exit(0)
path, dpi = sys.argv[1], float(sys.argv[2])
w, h = Image.open(path).size
print("Size: %dx%d px, %.1fmm square at %d dpi" % (w, h, w / dpi * 25.4, dpi))
PY

if [ "$VERIFY" = "0" ]; then
  echo "Verify: skipped (--print-only)"
  exit 0
fi

# Decode it back. Three decoders are tried because none of them is reliably
# present, and zbarimg 0.23.93 from Homebrew segfaults on qrencode's own
# palettised output on this machine, so it is deliberately not one of them.
set +e
python3 - "$OUT_PNG" "$QR_TEXT" <<'PY'
import sys, warnings
warnings.filterwarnings("ignore")
path, want = sys.argv[1], sys.argv[2]

try:
    from PIL import Image
except ImportError:
    print("Verify: NOT CHECKED (no Pillow). Scan the PNG with a phone before printing.")
    sys.exit(2)

img = Image.open(path).convert("RGB")
got, how = None, None

try:
    import zxingcpp
    r = zxingcpp.read_barcode(img)
    if r and r.valid:
        got, how = r.text, "zxing-cpp"
except Exception:
    pass

if got is None:
    try:
        from pyzbar.pyzbar import decode
        d = decode(img)
        if d:
            got, how = d[0].data.decode("utf-8"), "pyzbar"
    except Exception:
        pass

if got is None:
    try:
        import numpy, cv2
        text, _, _ = cv2.QRCodeDetector().detectAndDecode(numpy.array(img)[:, :, ::-1])
        if text:
            got, how = text, "opencv"
    except Exception:
        pass

if got is None:
    print("Verify: NOT CHECKED (no working QR decoder). Scan the PNG with a phone.")
    sys.exit(2)

if got == want:
    print("Verify: OK, decoded back to the same string with %s" % how)
    sys.exit(0)

print("Verify: FAILED. %s decoded %r, expected %r" % (how, got, want))
sys.exit(1)
PY
rc=$?
set -e

if [ "$rc" = "1" ]; then
  echo "Not printable. Delete $OUT_PNG and work out why before it reaches a card." >&2
  exit 1
fi
exit 0
