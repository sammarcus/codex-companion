# Printed card text

Copy for the small card that goes in each box. Two sides. Everything in
`[SQUARE BRACKETS]` is a placeholder to fill in before printing.

This is a gift, not a manual. The card's whole job is: say what the object is,
make clear it already works, and mention the optional extra without turning
the thing into homework. Keep it short enough to read while holding the unit
in the other hand.

---

## Front

```
Codex Desk Companion

A small screen that shows what your coding
session is doing, from across the room.
```

Design note, not printed copy: the front stays at those two lines. It should
read in under two seconds.

---

## Back

```
It already works.

Plug it into any USB port, any charger, any battery.
No app, no account, no WiFi, no setup. It lights up and
stays lit, and that is the whole object. If you never
read the rest of this card, nothing is missing.


Put your name on it

    ls /dev/cu.usbmodem*
    printf '{"name":"Your Name"}\n' > /dev/cu.usbmodem101

Use whichever path the first command printed. Up to 24
letters. It remembers, through unplugging and through
anything I do to it later.


Optional: make it show your live Codex session

There is one file at [URL]. Download it, run it, and the
screen starts showing what your session is doing: a ring
for how full your context window is, and a colour for
what is happening. The one worth having is the amber
pulse, which means your agent is waiting on you.

It is a single file with no dependencies. It installs
nothing and downloads nothing. It cannot approve or deny
anything on your behalf, ever: approvals still appear in
your terminal and you still answer them yourself.

    node codex-companion.js doctor     # look first
    node codex-companion.js            # run it, Ctrl-C stops it

What it does not do: no network of any kind, no
background service, nothing that starts itself, and it
never reads your prompts, your code, or your output.


Anything at all: [CONTACT]
```

---

## Notes for whoever prints this

Not printed. These are the checks behind each claim on the card.

- **"It already works" is literally true and is the most important line.**
  The firmware runs its own ambient animation forever on USB power with no
  host software anywhere (`firmware/AMBIENT.md`). This matters because the
  recipients are corporate employees who may never be allowed to, or want to,
  run anything on a work machine. The card must not read as though the unit is
  inert until they install something.
- **The naming commands were taken from the shipped protocol**, not invented.
  `name` is a real protocol field, capped at 24 characters, stored in the
  device's NVS, and reloaded on every boot (`setOwnerName` / `loadOwnerName`
  in `firmware/src/main.cpp`, and `docs/protocol.md` section 2.6). Two lines
  on the card rather than one, deliberately: a bare
  `> /dev/cu.usbmodem*` redirect is an ambiguous-redirect error in zsh the
  moment a second USB device is attached, which is a bad first experience.
- **"One file, no dependencies" is exact.** The helper is
  `helper/codex-companion.js`, a single file, and `helper/package.json` has no
  `dependencies` key at all. `npm install` in that directory installs nothing.
  Requires Node 20 or newer.
- **"Installs nothing" is exact.** The default command runs in the foreground
  and Ctrl-C ends it completely. There is no launchd agent, no LaunchAgent
  plist, no systemd unit, no login item, no cron entry, and no autostart of
  any kind anywhere in the file. The only thing it can ever write is
  `~/.codex/hooks.json`, and only if the recipient explicitly runs
  `install-hook`, which prints the exact bytes first and is undone by
  `uninstall-hook`. The card deliberately does not mention `install-hook`:
  running the program in the foreground is the honest one-line pitch, and
  anyone who wants the hook will find it in the program's own `help`.
- **"It cannot approve anything" is true twice over**, which is why the card
  says "ever" rather than "does not". The `PermissionRequest` handler writes
  nothing to stdout and exits 0, which is Codex's documented way to decline to
  decide; and it is registered `"async": true`, and Codex refuses to apply an
  allow or a deny from an async handler at all. Either mechanism alone is
  sufficient. See `helper/README.md`.
- **"Never reads your prompts, code or output" is checkable.** The hook reads
  exactly two fields of the payload (`hook_event_name` and `transcript_path`)
  and, for numbers only, the tail of the session file, where a substring test
  skips every line that is not token accounting or a turn boundary before
  `JSON.parse` ever sees it. Everything that reaches the screen is a number, a
  timestamp, or one of five fixed words.
- **The "what it does not do" line is on the card on purpose.** These are
  people who will reasonably wonder what a gift plugged into a work laptop is
  doing. Answering before they ask is the point, and the answer is short
  because it is genuinely short.
- **No npm, no npx, no package name anywhere.** Earlier drafts of this card
  printed `npm install -g ./codex-companion-1.0.0.tgz` and apologised at
  length for an unpublished package name. That entire situation is gone: there
  is no tarball to ship, no registry to check, and nothing to apologise for.
  Do not reintroduce a package install here.
- **`[URL]`** is the one real blocker. It has to be a place a coworker can
  fetch a single `.js` file from. Nothing is hosted today. Until it is
  decided, either fill it in, or hand the file over another way and rewrite
  that paragraph to match. Do not print a URL that does not resolve.
- **`[CONTACT]`** is whatever address Sam wants on fourteen physical cards
  handed to coworkers.
- **The QR code**, if you print one, is rendered by `docs/make-qr.sh` into
  `docs/out/card-qr.png`. It encodes `[URL]`, so the script refuses to run
  until that placeholder is replaced, on purpose: a QR that resolves to
  nothing is worse than no QR. It needs `qrencode` (`brew install qrencode`)
  and prints an install hint rather than failing silently. The QR is a
  convenience and the first thing to cut, see `docs/wednesday-runbook.md`
  section 7.
