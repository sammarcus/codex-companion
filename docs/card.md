# Printed card text

Copy for the small card that goes in each box. Everything in
`[SQUARE BRACKETS]` is a placeholder to fill in before printing.

This is a gift, not a manual. The card's whole job is: say what the object is,
make clear it already works, show the one thing worth doing to it, and mention
the optional extra without turning the thing into homework. Short enough to
read while holding the unit in the other hand.

The device gained a lot since the first draft of this card: a face, a first
run, three faces to choose from, a message card, a do-not-disturb sign, a
game, a stats screen and a focus timer. **The card did not grow to match.** The
buttons are a short table because the buttons are the whole manual, and
everything else is left to be found. A card that lists thirteen features reads
as a product; a card that says "it already works, here are the buttons" reads
as a gift.

---

## Front

```
Codex Desk Companion

A small creature for your desk. Add the optional
program and it tells you when Codex needs you.
```

Design note, not printed copy: the front stays at those two lines. It should
read in under two seconds, and **both lines have to be true on USB power
alone**. The earlier front read "A small screen that shows what your coding
session is doing, from across the room", which is the one thing the object
cannot do until somebody runs the program: `state` only ever arrives over the
wire, so with nothing attached there is no ring, no amber and not even a clock.
The correction was on the back, which is the half a busy person skips. The
front now leads with what it does unplugged and sells the upgrade in the second
line.

---

## Back

```
It already works.

Plug it into any USB port, any charger, any battery. No
app, no account, no WiFi, no setup. It wakes up, has a
look at you, and then it lives on your desk. If you
never read the rest of this card, nothing is missing.


Put your name on it

    ls /dev/cu.usbmodem*
    printf '{"name":"Your Name"}\n' > /dev/cu.usbmodemXXXX

Use whichever path the first command printed. If more
than one prints, unplug the board, run it again, and take
the path that vanished. Up to 24 plain letters, so write
accents out (Jose, Zoe). Nothing is printed back either
way, so the screen is the confirmation: your name is on
it within a second. It remembers, through unplugging and
through anything I do to it later.


Two buttons, and that is the whole manual

    1            answer the amber pulse
    1            your day
                 (both of those need the program below)
    1 twice      a focus timer, 5 to 45 minutes
    hold 1       a small game, for a turn that runs long
    2            brightness
    hold 2       a different face
    both         DO NOT DISTURB, for the whole room
    hold both    forget everything, start over
                 (not while your computer is talking to it)

While the screen is pulsing amber, button 1 only answers it.
The other button 1 screens wait until it is cleared.

Button 2 gets you out of anything.

If the screen stays black, unplug it and plug it back in
without touching the buttons.


Optional: your live Codex session

One file, at [URL]. Run it and the screen starts showing
what your session is doing: a ring for how full your
context window is, and an amber pulse that means your
agent is waiting on you. That one is worth having.

    node codex-companion.js doctor     # look first
    node codex-companion.js            # run it, Ctrl-C stops it

It is one file with no dependencies, it installs nothing
and it starts nothing. It never approves or denies
anything, never opens a network connection, and never
reads your prompts, your code or your output.


Anything at all: [CONTACT]
```

---

## Notes for whoever prints this

Not printed. These are the checks behind each claim on the card.

- **"It already works" is literally true and is still the most important
  line.** With no host software anywhere the unit plays its out-of-the-box
  sequence once (`firmware/FIRSTRUN.md`) and then runs ambient mode forever
  (`firmware/AMBIENT.md`). The recipients are corporate employees who may
  never be allowed to, or want to, run anything on a work machine, so the card
  must not read as though the unit is inert until they install something.
- **"It wakes up, has a look at you" is the first run, not a metaphor.** An
  unopened board is dark for two seconds, a light comes up on a pair of shut
  eyes, they crack open, look around, find the person in front of them, blink,
  and say whose it is. 8.6 seconds, once. Timed beat by beat in
  `firmware/FIRSTRUN.md` section 1. It is the reason the card promises
  something happens the moment they plug it in.
- **The naming commands were taken from the shipped protocol** and both were
  run against a real board on 2026-09-08 (the form that was run used that
  board's real path, `/dev/cu.usbmodem1101`; the card prints
  `/dev/cu.usbmodemXXXX` instead, deliberately). `name` is a real protocol
  field, capped at 24 characters, stored in NVS and reloaded on every boot
  (`docs/protocol.md` section 2.6). Two lines rather than one, deliberately: a
  bare `> /dev/cu.usbmodem*` redirect is an ambiguous-redirect error in zsh the
  moment a second USB device is attached, and on the machine this was written
  on `ls /dev/cu.usbmodem*` returns two ports, the second of which is a USB
  power meter. **The example path is deliberately unusable as written.** An
  earlier draft printed a real path, `/dev/cu.usbmodem1101`, and
  `site/index.html` printed a different real one, `/dev/cu.usbmodem101`, on the
  page this card points at. Two plausible paths on two artifacts a person reads
  back to back is a way to get a silent no-op instead of a named board, so both
  now print `XXXX`, which cannot be copied blind. `README.md` already used that
  form.
- **"Up to 24 letters" was measured, not estimated.** A 30 character name sent
  over the wire came back from the board's own greeting as
  `name="ABCDEFGHIJKLMNOPQRSTUVWX"`, exactly 24. ASCII only: the built-in
  LovyanGFX bitmap fonts have no glyphs past it.
- **"Plain letters, so write accents out" is on the card because the failure is
  silent, and across fourteen colleagues at least one accented name is close to
  certain.** `setOwnerName` runs `saySanitize` first, which drops every byte
  outside `0x20..0x7E`. Proved on the bench board on 2026-09-08, sending the
  card's own literal form and then resetting to read the greeting back:
  `{"name":"José Ruiz"}` came back `name="Jos Ruiz"`, `{"name":"Zoë"}`
  came back `name="Zo"`, and a two-character Chinese name was answered
  `err: name refused, nothing printable in it` with the previous name left in
  place. Two different wrong outcomes, neither of them visible to somebody
  following the card, because the card's instruction is a one-way shell
  redirect and the `err:` line has nowhere to go. Hence the other new sentence:
  **the screen is the confirmation**. Adding a third command to read the port
  back was the alternative and it costs more card than it is worth; telling
  somebody to look at the object in their hand does not.
- **"If more than one prints, unplug the board, run it again, and take the path
  that vanished."** The note above already records that this machine returns two
  `/dev/cu.usbmodem*` ports, the board and a USB power meter, and the card used
  to say "use whichever path the first command printed", singular, with no way
  to tell them apart. Picking the wrong one is completely silent: a redirect to
  a power meter is not an error. Checked while writing this: `ls -la
  /dev/cu.usbmodem*` returned `/dev/cu.usbmodem0054452` and
  `/dev/cu.usbmodem1101`. The helper's `doctor` resolves this properly, but the
  naming step is deliberately in front of the optional program, so the card has
  to settle it on its own.
- **Every row of the button table is a shipped gesture**, and the table is the
  current one (`firmware/README.md`, "The button model, again"). One tap of
  button 1 is the stats screen, a second tap goes on to the focus timer, a two
  second hold of button 1 is the game, one tap of button 2 is brightness, an
  800ms hold of button 2 is the next face, both buttons tapped together is the
  do-not-disturb sign, and both held for five seconds is a factory reset.
- **The first row is the hero state, and the card used to leave it out
  entirely.** While the screen is pulsing amber, a press of button 1 is the
  acknowledgement and nothing else: `pumpButton` spends that press on the ack
  on the PRESS edge and marks it `button1Held`, which swallows the release and
  the hold too. So every other button 1 row on this table is inert for exactly
  as long as the pulse is up, and the firmware says so on the wire: with
  `{"state":"waiting"}` set on the bench board, `{"stats":"on"}` answered
  `err: stats refused, prompt pending`, `{"play":"on"}` answered
  `err: play refused, prompt pending`, and `{"focus":"show"}` answered
  `err: focus refused, prompt pending`. The refusals are the right behaviour
  and none of them needs changing. What was wrong was the card: it printed
  three button 1 screens and never printed the one interaction the whole object
  exists for. `firmware/README.md`, "The button model, again", carries the same
  rule. The two lines under the table are there so a recipient whose taps do
  nothing knows why, rather than deciding the buttons are broken.
- **"Button 2 gets you out of anything" is the device's actual cross-cutting
  rule, and the card used to print a false one.** It said "any button", which
  is true on the message card, the sign and the timer's finish and false on the
  other three modal screens: on the toy button 1 hops (`toyHop`), on the stats
  screen button 1 goes *on* to the focus timer (`statsExit` then `focusShow`),
  and on the timer itself button 1 is the run control (`focusToggle`, and a
  hold resets the clock). A recipient told "any button leaves" who presses
  button 1 to leave the timer starts a 25 minute pomodoro instead, then pauses
  it, then resets it. Button 2 is verified to leave on every one of the six:
  say, sign, toy, stats, timer, timer finish. It is also what every on-screen
  legend already says ("2 CLOSE", "2 EXIT"), it is what `firmware/README.md`
  states ("On any modal screen, button 2 leaves"), and it makes the whole
  two-button model one line: **button 1 acts, button 2 closes.**
- **"If the screen stays black, unplug it and plug it back in without touching
  the buttons" is on the card because of a real, silent failure mode.** Button
  1 is GPIO 0, which is the ESP32-S3 BOOT strapping pin: holding it while USB
  power arrives boots the chip into the ROM download mode instead of the
  firmware. The panel stays black, nothing is printed, and the board sits
  waiting for a flasher forever. It is easy to reach by accident, because a
  hand holding the board by its top edge while pushing in a USB-C plug is
  holding button 1, and because `firmware/FIRSTRUN.md` opens with two seconds
  of deliberate darkness, so a person who does it waits, assumes it is booting,
  and never gets a picture. The card's own button table teaches a two second
  hold of button 1 as an ordinary thing to do, so it has already told them
  holding that button is normal. This is
  the only failure on this device with no on-screen rescue at all, and the
  recovery is a replug with no buttons touched. Eleven words covers it and
  every other cold-boot oddity. The deliberate way to enter that mode is
  documented in `firmware/README.md` and `docs/wednesday-runbook.md` section 6:
  hold BOOT, tap RST.
- **The stats row promises "your day" and says where the day comes from.** It
  used to read "your day: turns, tokens, longest turn", and on a board with no
  host software every one of those figures is zero: `stTurns` only increments
  inside the `ST_DONE` branch of `statsOnState`, `stTokens` is written only by
  `statsAddTokens` off the optional `tokens` field, `stSessionOpen` is set only
  by `statsHostSpoke`, and without the `time` field the header reads
  `SINCE BOOT` rather than `TODAY`. Proved on the bench board, 2026-09-08:
  after a hardware reset, with no `state`, `tokens` or `time` line ever sent,
  `{"stats":"on"}` answered
  `stats: open (host) turns=0 tokens=0 longest=0s ... day=since-boot`. On the
  panel that is TURNS 0, TOKENS 0, LONGEST 0:00, BEST TURN 0:00, under a
  `SINCE BOOT` header. (`SESSION` reads `--` only when nothing is attached at
  all: `stSessionOpen` is set by `statsHostSpoke`, and a probe over the cable
  is itself a host, so the run above reported an open session.) Design law 1
  says ambient with no
  host is what most owners will see most of the time, and button 1 is the
  physically obvious one, so the first thing a recipient does must not be a
  screen of zeros with no explanation. The row now names the dependency in four
  words. The screen itself is unchanged and everything else in the table works
  standalone, including the focus timer one tap further on.
  **The caveat now covers the row above it too.** The amber pulse is host-only
  in exactly the same way: there is no path to `ST_WAITING` without a
  `{"state":"waiting"}` line, so on the majority path "answer the amber pulse"
  was dead copy sitting above the only row that carried a warning. The two rows
  now share one note, which also makes the standalone gestures read as the
  default and the host-fed ones as the bonus.
- **One thing on that table is destructive and the card says so in the mildest
  possible words.** "Forget everything, start over" is `Preferences::clear()`
  on the whole namespace: the name, the face, the first-run flag, the game's
  best score, the stats records and the focus timer's chosen length (the full
  list is `grep -n NVS_KEY_ firmware/src/main.cpp`). It also replays the first
  run, which is why
  it is worth having on the card at all: it is the only way a recipient can see
  the out-of-the-box sequence a second time. It needs five seconds of both
  buttons in ambient mode, which is not a thing a hand does by accident
  (`firmware/FIRSTRUN.md` section 4). **The row's second line is on it because
  the ambient-mode condition is real and silent**: the gesture is gated on
  `b2Down && ambientMode && held >= FACTORY_HOLD_MS`, and live mode persists
  for five minutes after the last line a host sent, so a recipient running the
  optional program can hold both buttons for five seconds and get nothing at
  all, with no message on the panel and no notice on the wire. It used to read
  "(unplug first)", which was wrong advice for most of the people holding this
  card. `ambientMode` is already true on any board that has never met a host
  (`firmware/AMBIENT.md` section 1: "No protocol line has ever been accepted
  since boot"), and design law 1 plus this card's own opening line say that is
  most owners. For them there is nothing to unplug from except power, and
  cutting power is exactly what makes a five second button hold impossible. The
  precondition is a computer talking to the board, not a cable, so the row now
  says that. **An earlier version of this note claimed `site/index.html` had
  always said the same thing. It had not**: until 2026-09-08 that page's
  factory-reset row ended "so unplug the cable from the computer first", the
  exact advice this paragraph calls wrong, on the page this card points at. It
  now matches the card. If you are proofreading, check the page rather than
  trusting a note like this one. The gate itself is a good
  decision and should stay: with a session in flight there is no name and no
  state worth destroying.
- **The card deliberately does not mention** the message card (`say`), the
  do-not-disturb sign's protocol door (`dnd`), the three faces by name, the
  milestone pills, the focus timer's protocol fields, or `install-hook`. All of
  them are real and all of them are documented in `docs/protocol.md`. None of
  them survives the test of "would a person holding this in one hand read
  another line".
- **"One file, no dependencies" is exact.** The helper is
  `helper/codex-companion.js`, a single file, and `helper/package.json` has no
  `dependencies` key at all. `npm install` in that directory installs nothing.
  Requires Node 20 or newer, and macOS in practice, since port discovery reads
  `ioreg` (`docs/open-questions.md`, N7).
- **"Installs nothing, starts nothing" is exact.** The default command runs in
  the foreground and Ctrl-C ends it completely. There is no launchd agent, no
  plist, no systemd unit, no login item, no cron entry and no autostart of any
  kind anywhere in the file. The only thing it can ever write is
  `~/.codex/hooks.json`, and only if the recipient explicitly runs
  `install-hook`, which prints the exact bytes first and is undone by
  `uninstall-hook`. The card deliberately does not mention `install-hook`:
  running the program in the foreground is the honest one-line pitch, and
  anyone who wants the hook will find it in the program's own `help`.
- **"It never approves or denies anything" is true twice over.** The
  `PermissionRequest` handler writes nothing to stdout and exits 0, which is
  Codex's documented way to decline to decide; and it is registered
  `"async": true`, and Codex refuses to apply an allow or a deny from an async
  handler at all. Either mechanism alone is sufficient. See `helper/README.md`.
- **"Never opens a network connection" is checkable in one grep.** No
  networking module is loaded anywhere in the file. `helper/README.md` prints
  the exact greps, and they are meant to be run by whoever has to approve this
  on a work laptop.
- **"Never reads your prompts, your code or your output" is checkable too.**
  The hook reads exactly two fields of the payload (`hook_event_name` and
  `transcript_path`) and, for numbers only, the tail of the session file, where
  a substring test skips every line that is not token accounting or a turn
  boundary before `JSON.parse` ever sees it. Everything that reaches the screen
  is a number, a timestamp, or one of five fixed words.
- **Those three "never" claims are one line on the card on purpose.** These are
  people who will reasonably wonder what a gift plugged into a work laptop is
  doing. Answering before they ask is the point, and the answer is short
  because it is genuinely short.
- **No npm, no npx, no package name anywhere.** Earlier drafts printed
  `npm install -g ./codex-companion-1.0.0.tgz` and apologised at length for an
  unpublished package name. There is no tarball to ship, no registry to check
  and nothing to apologise for. Do not reintroduce a package install here.
- **`[URL]`** is the one real blocker (`docs/open-questions.md`, N1). It has to
  be a place a coworker can fetch a single `.js` file from. **A landing page
  now exists in the tree**, `site/index.html`, a single self-contained file that
  names `codex-companion.js` and carries no external links yet. It is not hosted
  anywhere: as of 2026-09-08 the only URLs in it are in-page anchors. So `[URL]`
  is whatever that page's address turns out to be, and it is still a decision
  nobody has made. Until it is decided, either fill it in, or hand the file over another
  way and rewrite that paragraph to match. Do not print a URL that does not
  resolve. Keep it short: measured with `docs/make-qr.sh`, a 38 character URL
  renders as a 31mm QR and an 88 character one as a 42mm QR, and only one of
  those fits comfortably on a card.
- **`[CONTACT]`** is whatever address Sam wants on fourteen physical cards
  handed to coworkers.
- **The QR code**, if you print one, comes from `docs/make-qr.sh`, which writes
  both `docs/out/card-qr.png` (opaque white, 300 dpi) and
  `docs/out/card-qr.svg`. It encodes the same string as `[URL]` and refuses to
  render while that is unset, on purpose: a QR that resolves to nothing is
  worse than no QR, and it is the one error nobody can spot on a printed card.
  You no longer have to edit the script to set it:

      docs/make-qr.sh "https://example.com/codex-companion.js"

  It decodes its own output back and tells you whether it matched. It needs
  `qrencode` (`brew install qrencode`) and prints that hint rather than failing
  silently. The QR is a convenience and the first thing to cut, see
  `docs/wednesday-runbook.md` section 8.
