# Printed card

## Front

```
Codex Desk Companion

A small creature for your desk. Add the optional
program and it tells you when Codex needs you.
```

## Back

```
It already works.

Plug it into any USB port, any charger, any battery.
No app, no account, no setup.


Put your name on it

    ls /dev/cu.usbmodem*
    printf '{"name":"Your Name"}\n' > /dev/cu.usbmodemXXXX

Use whichever path the first command printed.


Optional: your live Codex session, on the screen

    npx github:sammarcus/codex-companion


Read more: sa.mmarc.us/codex-companion.html

Anything at all: sam.marcus@me.com
```

## Not printed: notes for whoever prints it

- Finished size: business card, 89 x 51 mm, front and back, exactly the two blocks above.
- The QR goes on the back. It is `docs/out/card-qr.png`, 370 px square, opaque white, about 31 mm at 300 dpi. `card-qr.svg` sits beside it for a printer that wants vector.
- It encodes `https://sa.mmarc.us/codex-companion.html`, the same address printed above it, so a phone's scan preview shows the host the card names. No shortener: a stranger's QR that previews an unfamiliar redirector is one people are trained to back out of.
- Regenerate with `docs/make-qr.sh` (needs `qrencode`; it takes the URL from `docs/card-url.txt`, already filled in, and decodes its own output back to check it).
- The naming step is two commands because a bare `> /dev/cu.usbmodem*` is an ambiguous redirect in zsh once a second USB device is attached.
- Names are plain ASCII and capped at 24 characters. Accents are dropped silently, so the screen is the confirmation.
- The QR is the first thing to cut if the layout gets tight. The card works as plain text.
