# Printed QR card text

Text for the small card that ships in the box with each unit. Two sides,
front and back. Render the QR itself with `docs/make-qr.sh` (see bottom of
this file).

Everything in brackets is a placeholder to fill in before printing.
Everything else is confirmed against `helper/package.json` and
`helper/src/install.js`, both of which exist and were read directly, this
card no longer carries the "package name/CLI verbs not yet decided"
hedging an earlier draft had, since that got settled by the actual build.

---

## Front

```
Codex Desk Companion

Shows your live Codex CLI session at a glance.
```

Design note (not printed copy): keep the front to just those two lines.
This is a status object on a desk, not a poster, the second line should
read in under two seconds.

---

## Back

```
Set up (one time):

    npx codex-companion install

Plug the unit into a USB port on this Mac first, then run that one
command in a terminal. It survives reboots, you won't need to run it
again.

To remove:

    npx codex-companion uninstall

Questions or something's broken? sam.marcus@me.com
```

Notes on the back copy, for whoever finalizes this before printing:

- **`codex-companion` is the confirmed, real npm package name.**
  `helper/package.json`'s `"name"` field and `"bin": {"codex-companion":
  "bin/codex-companion.js"}` both read `codex-companion` directly, and
  `docs/host-tooling-recon.md` section 1 independently confirmed that name
  free on the npm registry with no prior-publish history (unlike this
  repo's own directory name, `codex-buddy`, which came back risky). The
  only thing not yet confirmed is that someone has actually run `npm
  publish` under this name, if it's still unpublished when cards go to
  print, `npx codex-companion install` will fail with a registry 404, not
  quietly do something else, worth a final `npm view codex-companion`
  check the morning cards get printed.
- **`install` and `uninstall` are the real, confirmed subcommands.**
  `helper/src/install.js`'s `main()` switches on exactly these two names
  (plus `run`, `status`, `doctor`, `demo`, `help`), read directly from
  source. Bare `npx codex-companion` (no subcommand) also works, it
  defaults to `install` followed immediately by `run` in the foreground
  (`install.js`, the `case 'default'` branch), but the explicit `install`
  form is clearer for a printed card aimed at someone who has never seen
  this tool before.
- Install copies the package into a **stable, persistent** directory,
  `~/.codex-companion/app` (`installDir()`, `install.js:31-33`), not an
  `npx` on-demand cache path, and on macOS registers a launchd LaunchAgent
  from `helper/templates/com.codex-companion.plist` with `KeepAlive` and
  `RunAtLoad` both true, this is what "survives reboots" on the card
  actually refers to, confirmed against the template file itself. Linux
  gets an analogous `systemd --user` unit (`renderSystemdUnit`,
  `install.js:82-91`); Windows gets printed instructions only, no
  autostart is set up automatically there (`install.js`'s own top comment).
  If any of the 14 recipients are on Linux or Windows, the "survives
  reboots" line on their card may not hold as written, flag this before
  printing if that's a real possibility for this batch.
- The "plug in first, then run the command" ordering matters: the
  installer needs to find the device's serial port
  (`helper/src/device.js`, matching on `vendorId === '303a'` with a
  manufacturer/`pnpId` string fallback, confirmed at `device.js:21,61-71`)
  to do anything useful.
- `sam.marcus@me.com` is a placeholder contact and should stay as-is
  unless Sam wants a different address on 14 physical cards handed to
  coworkers.

---

## QR code

`docs/make-qr.sh` renders a PNG of the **exact setup command** from the
back of the card (not a URL, since there is no hosted page or public repo
for this project yet, confirmed by `git remote -v` returning nothing in
this repo). Scanning the code should let someone paste the command
straight into a terminal instead of typing it. If a public repo or docs
page for this project exists by the time cards are printed, switching the
QR payload to that URL instead of the raw command is a one-line edit to
`make-qr.sh` (the `QR_TEXT` variable at the top), not a rewrite.

Run:

```bash
docs/make-qr.sh
```

Output: `docs/out/card-qr.png`. The script checks for `qrencode` first
(`which qrencode`) and, if it's missing, prints `brew install qrencode`
instead of failing silently. Confirmed on this machine, 2026-09-07:
`qrencode` is **not currently installed** here (`which qrencode` returned
nothing, and running the script produced exactly the install-hint output,
exit code 1, no PNG), so running the script as-is right now will print the
install hint rather than produce a PNG until `brew install qrencode` is
run first.
