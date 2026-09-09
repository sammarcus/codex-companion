# Printed card text

Copy for the small card that goes in each box. Everything in
`[SQUARE BRACKETS]` is a placeholder to fill in before printing.

This is a gift, not a manual. The card's whole job is: say what the object is,
make clear it already works, show the one thing worth doing to it, and mention
the optional extra without turning the thing into homework. Short enough to read
while holding the unit in the other hand.

The device gained a lot since the first draft of this card: a face, a first run,
three faces, a message card, a do-not-disturb sign, a game, a stats screen and a
focus timer. **The card did not grow to match.** The buttons are a short table
because the buttons are the whole manual, and everything else is left to be
found. A card that lists thirteen features reads as a product; a card that says
"it already works, here are the buttons" reads as a gift.

---

## Front

```
Codex Desk Companion

A small creature for your desk. Add the optional
program and it tells you when Codex needs you.
```

Both lines have to be true on USB power alone, which is why the front leads with
what it does unplugged and sells the upgrade second. An earlier front read "a
small screen that shows what your coding session is doing", which is the one
thing the object cannot do until somebody runs the program.

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

One file, at github.com/sammarcus/codex-companion. Run it and the screen starts showing
what your session is doing: a ring for how full your
context window is, and an amber pulse that means your
agent is waiting on you. That one is worth having.

    node codex-companion.js doctor     # look first
    node codex-companion.js            # run it, Ctrl-C stops it

It is one file with no dependencies, it installs nothing
and it starts nothing. It never approves or denies
anything, never opens a network connection, and never
reads your prompts, your code or your output.


Anything at all: sam.marcus@me.com
```

---

## Notes for whoever prints this

Not printed. These are the non-obvious calls behind the copy, each of which
started as a bug in an earlier draft.

**The example path is deliberately unusable.** `/dev/cu.usbmodemXXXX` cannot be
copied blind. An earlier draft printed a real path and `site/index.html` printed
a *different* real one, and two plausible paths on two artifacts somebody reads
back to back is a way to get a silent no-op instead of a named board. It is two
commands rather than one because a bare `> /dev/cu.usbmodem*` redirect is an
ambiguous-redirect error in zsh the moment a second USB device is attached, and
this machine returns two ports: the board and a USB power meter. Picking the
wrong one is completely silent, because a redirect to a power meter is not an
error, hence the "take the path that vanished" sentence.

**"Plain letters, so write accents out" is on the card because the failure is
silent**, and across fourteen colleagues at least one accented name is close to
certain. The name goes through the same sanitiser the message card uses, which
drops every byte outside 0x20..0x7E. Proved on the bench board: `{"name":"José
Ruiz"}` came back `name="Jos Ruiz"`, `{"name":"Zoë"}` came back `name="Zo"`, and
a two-character Chinese name was answered `err: name refused, nothing printable
in it` with the previous name left in place. Two different wrong outcomes,
neither visible to somebody following a one-way shell redirect. Hence the other
sentence: **the screen is the confirmation.** The 24 character cap was measured,
not estimated: a 30 character name came back from the greeting as exactly 24.

**"Button 2 gets you out of anything" is the device's actual cross-cutting
rule.** The card used to say "any button", which is true on the message card,
the sign and the timer's finish and false on the other three: on the toy button
1 hops, on the stats screen it goes *on* to the focus timer, and on the timer it
is the run control. A recipient told "any button leaves" who presses button 1 to
leave the timer starts a 25 minute pomodoro instead, then pauses it, then resets
it. Button 2 is verified to leave on all six, it is what every on-screen legend
already says, and it makes the whole model one line: **button 1 acts, button 2
closes.**

**"If the screen stays black, unplug it and plug it back in without touching the
buttons" covers a real, silent failure mode.** Button 1 is GPIO 0, the ESP32-S3
BOOT strapping pin: holding it while USB power arrives boots the chip into ROM
download mode instead of the firmware. The panel stays black, nothing is
printed, and the board waits for a flasher forever. It is easy to reach by
accident, because a hand holding the board by its top edge while pushing in a
USB-C plug is holding button 1, and because the first run opens with two seconds
of deliberate darkness, so a person who does it waits, assumes it is booting,
and never gets a picture. This is the only failure on this device with no
on-screen rescue at all.

**The first two rows carry a shared caveat because both are host-only.** There
is no path to the amber pulse without a `{"state":"waiting"}` line, and on a
board with no host software every figure on the stats screen is zero under a
`SINCE BOOT` header. Design law 1 says ambient with no host is what most owners
see most of the time, and button 1 is the physically obvious one, so the first
thing a recipient does must not be a screen of zeros with no explanation.
Everything else in the table works standalone, including the focus timer one tap
further on.

**"Forget everything, start over" is the one destructive row**, and its second
line is on it because the ambient-mode precondition is real and silent: the
gesture is gated on ambient mode, live mode persists for five minutes after the
last line a host sent, and a recipient running the optional program can hold
both buttons for five seconds and get nothing at all, with no message on the
panel and no notice on the wire. The precondition is a computer talking to the
board, not a cable, which is why the row does not say "unplug first". It is on
the card at all because it replays the first run, and that is the only way a
recipient can see the out-of-the-box sequence a second time.

**The card deliberately does not mention** the message card, the sign's protocol
door, the three faces by name, the milestone pills, the focus timer's protocol
fields, or `install-hook`. All are real and all are in `docs/protocol.md`. None
survives the test of "would a person holding this in one hand read another
line". Likewise: **no npm, no npx, no package name anywhere.** Earlier drafts
printed an install command for an unpublished tarball. There is nothing to
install. Do not reintroduce it.

**The three "never" claims are one line on purpose.** These are people who will
reasonably wonder what a gift plugged into a work laptop is doing. Every one is
checkable in a grep, and `docs/verification.md` prints the greps, meant to be run
by whoever has to approve this on a work machine.

**`github.com/sammarcus/codex-companion`** is the one real blocker (`docs/open-questions.md`, N1). It has to be
a place a coworker can fetch a single `.js` file from. `site/index.html` exists
in the tree and is not hosted anywhere. Do not print a URL that does not
resolve. Keep it short: measured with `docs/make-qr.sh`, a 38 character URL
renders as a 31mm QR and an 88 character one as a 42mm QR, and only one of those
fits comfortably on a card.

**`sam.marcus@me.com`** is whatever address goes on fourteen physical cards.

**The QR**, if you print one, comes from `docs/make-qr.sh`, which writes
`docs/out/card-qr.png` (opaque white, 300 dpi) and `docs/out/card-qr.svg`. It
encodes the same string as `github.com/sammarcus/codex-companion` and **refuses to render while that is unset**,
on purpose: a QR that resolves to nothing is the one error nobody can spot on a
printed card. It decodes its own output back and tells you whether it matched.
Needs `qrencode`. The QR is the first thing to cut on flash day.
