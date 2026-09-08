# Licences

What third-party material this project touched, under what licence, and
exactly what was and was not taken from each.

Every licence below was read from the file on disk, not looked up. The path
and the commit are given so anyone can check the same bytes.

## The one-paragraph summary

`vendor/` is gitignored and is **not published**. It holds four shallow clones
that were read while building this project; none of their source, assets or
documents are redistributed here. What travelled out of them into this repo is
facts (GPIO pin numbers, panel offsets, event names, record shapes) and design
reasoning, all of it cited in place.

Two real obligations survive that, and both are about the **flashed firmware
binary** rather than about the repo:

1. **LovyanGFX** (BSD-2-Clause) and **ArduinoJson** (MIT) are linked into
   `firmware.bin`. Both licences require their copyright notice to accompany a
   binary redistribution. This file is that notice; see section 6.
2. The **Arduino core for ESP32** is LGPL-2.1-or-later and is also linked in.
   See section 6 for what that means for handing somebody a flashed board.

Nothing else in the tree carries an obligation, and nothing unlicensed has
been copied.

---

## 1. `vendor/codex` (OpenAI Codex)

| | |
|---|---|
| Upstream | `https://github.com/openai/codex` |
| Commit read | `121f91fd5d9dc66017866ce9bdc49f1e182721df` |
| Licence | **Apache License 2.0** (`vendor/codex/LICENSE`, read in full) |
| Notice file | `vendor/codex/NOTICE`: "OpenAI Codex, Copyright 2025 OpenAI", and a note that the project includes Ratatui code under MIT (Copyright 2016-2022 Florian Dehau, 2023-2025 The Ratatui Developers) |
| Published here | **No.** Gitignored. |

### What we took

Facts about an interface we talk to, not code:

- **The twelve hook event names** and their spelling, in `ALL_HOOK_EVENTS`
  (`helper/codex-companion.js`).
- **The dispatch rules**: that Codex will not apply an allow or a deny from a
  handler whose execution mode is async (`hooks/src/engine/mod.rs`,
  `can_apply_control_effects`), that async handlers are scheduled and not
  waited on (`hooks/src/engine/dispatcher.rs`), and that `SessionEnd` always
  runs synchronously with a 3 second cap (`hooks/src/events/session_end.rs`).
  Those three facts are load bearing: they are why this program provably
  cannot approve anything and provably cannot block a session.
- **The `PermissionRequest` convention** that an empty stdout with exit 0 is
  the documented way to decline to decide
  (`hooks/src/events/permission_request.rs`, the `trimmed_stdout.is_empty()`
  branch of `parse_completed`).
- **Rollout record type names and field shapes**, which drive `METRIC_HINTS`
  and `applyUsage` in `helper/codex-companion.js` and are written up in
  `docs/codex-state-format.md`.
- **The context-fill formula.** `contextFill()` reimplements Codex's own
  baseline-adjusted calculation (`protocol.rs`,
  `percent_of_context_window_remaining`) so the ring agrees with the number
  the Codex TUI shows. This is the closest thing here to a derivation rather
  than a fact: five lines of arithmetic, written from the described behaviour
  rather than pasted. Apache-2.0 permits copying it outright in any case; the
  citation is in the function's own doc comment and stays there.
- **`helper/test/fixtures/session-sample-synthetic.jsonl`** is hand assembled
  from `codex-rs/protocol/src/protocol.rs` and `items.rs` at the commit above.
  Field names and types are taken from those structs; every value in it is
  invented. Its first line says so.

### What we did not take

No Rust source, no TUI code, no Ratatui code, no assets, no build files. There
is no Codex code in `firmware/` or `helper/`.

### Obligation

Apache-2.0 attaches its attribution and NOTICE conditions (section 4) to
redistribution of the Work or of Derivative Works. We redistribute neither.
The citations above are kept because they make the security claims checkable,
not because a licence forces them.

---

## 2. `vendor/LovyanGFX`

| | |
|---|---|
| Upstream | `https://github.com/lovyan03/LovyanGFX` |
| Commit read | `45dc36beb61f67dfd33a19954a1227acb02cad25`, tag `1.2.28` |
| Licence | `vendor/LovyanGFX/license.txt`, which is **four** licences stacked, read in full |
| Published here | **No.** Gitignored. **But the library is linked into the shipped firmware binary.** See section 6. |

`license.txt` contains, in order:

| Component | Licence | Copyright |
|---|---|---|
| Adafruit_ILI9341 original header | MIT ("all text above must be included in any redistribution") | Limor Fried / Ladyada, Adafruit Industries |
| Adafruit_GFX original library | BSD 3-Clause | 2012 Adafruit Industries |
| TFT_eSPI original library | FreeBSD / BSD 2-Clause | 2020 Bodmer |
| LovyanGFX itself | FreeBSD / BSD 2-Clause | 2020 lovyan03 |

### What we took

**No source.** `firmware/src/LGFX_TDisplayS3.hpp` is a hand-written
`lgfx::LGFX_Device` subclass, written against the library's public
configuration API because LovyanGFX ships no `boards.hpp` autodetect entry for
this board variant. The vendored clone was read to find out what that API is.

The library itself is not vendored into the build at all: PlatformIO fetches
`lovyan03/LovyanGFX@1.2.28` from its own registry
(`firmware/platformio.ini`, `lib_deps`). The clone in `vendor/` and the copy
PlatformIO downloads into `firmware/.pio/libdeps/` are two separate things,
and both are gitignored.

### What we did not take

No files, no examples, no fonts beyond the ones compiled in by the library
itself, no images, no documentation text.

### Obligation

BSD-2-Clause condition 2: a redistribution **in binary form** must reproduce
the copyright notice, the conditions and the disclaimer "in the documentation
and/or other materials provided with the distribution". `firmware.bin` is a
binary redistribution of LovyanGFX. This file is the documentation that
accompanies it. See section 6 for the notice text.

---

## 3. `vendor/T-Display-S3` (LILYGO board SDK)

| | |
|---|---|
| Upstream | `https://github.com/Xinyuan-LilyGO/T-Display-S3` |
| Commit read | `ec889e789b3cf093412689a143f7f37b42b56af7` |
| Licence | **MIT**, Copyright (c) 2022 Xinyuan-LilyGO (`vendor/T-Display-S3/LICENSE`, read in full) |
| Published here | **No.** Gitignored. |

### What we took

Hardware facts, all of them cited line by line in `docs/hardware-recon.md` and
restated in `firmware/src/LGFX_TDisplayS3.hpp`:

- The GPIO map, from `examples/factory/pin_config.h`: data pins
  39, 40, 41, 42, 45, 46, 47, 48; `CS 6`, `DC 7`, `WR 8`, `RD 9`, `RES 5`,
  `BL 38`, `POWER_ON 15`, buttons `0` and `14`.
- Bus, panel and backlight configuration values, from
  `examples/T-Display-S3-Queue/ST7789_Handler.h`: 16MHz write clock,
  `offset_rotation = 1`, `offset_x = 35`, `invert = true`, panel 170x320,
  backlight PWM at 22kHz on channel 7.
- The board's own rotation convention, counted across `examples/*/*.ino`.
- The panel spec line from its `README.md`.

These are the electrical facts of a physical object. MIT would permit copying
the header files outright with the notice attached; we did not need to,
because the values are all that was wanted.

### What we did not take, and must not

`vendor/T-Display-S3/` also contains material that **LilyGO redistributes but
does not own**, and that its MIT `LICENSE` therefore does not cover:

- `datasheet/ST7789V_SPEC_V1.4.pdf` (Sitronix), and the two Hynitron touch
  controller documents `CST328` and `CST816S`. Vendor datasheets, redistributed
  by a board maker. **Do not copy these into this repo and do not republish
  them.** Values read out of a datasheet are facts and are fine; the document
  is not.
- `schematic/*.pdf`, `dimensions/*` (STEP, STL, DXF, DWG, and a third-party
  contributed enclosure). Board artwork and CAD. Same rule.
- `image/` product photography.

None of it is in this repo, and the `vendor/` gitignore is what keeps it that
way.

---

## 4. `vendor/claude-desktop-buddy` (design reference only)

| | |
|---|---|
| Upstream | `https://github.com/anthropics/claude-desktop-buddy` |
| Commit read | `a280c6421931431ba6905aee9d2b50b2bfd8c103` |
| Licence | **MIT**, Copyright 2026 Anthropic, PBC (`vendor/claude-desktop-buddy/LICENSE`, read in full) |
| Published here | **No.** Gitignored. |

**This one is design reference and nothing else.** It is a different device
(M5StickC Plus, 135x240, GIF and ASCII pet characters, BLE) and a different
product. `docs/design-spec.md` says so in its own opening paragraph, and Part 2
of that document states outright that it is "a new design informed by Part 1,
not a port of it".

### What we took

Observations, written up with a file and line citation each, in
`docs/design-spec.md` Part 1: its seven-state enum and the priority order of
its `derive()` function, its timing constants, its "impatience" escalation
behaviour, its accent colour table, its landscape clock-face layout, and its
full-screen-sprite render pattern. Part 1 also flags a discrepancy between that
project's README and its code and deliberately leaves it unresolved, because
resolving it was not ours to do.

Everything in Part 2 is this project's own design. The three faces
(`rounded`, `bear`, `arc`) are original drawings built from primitives; none of
them is a port of a character from that repo.

### The carve-out: the bufo GIFs are NOT MIT and must not be copied

`vendor/claude-desktop-buddy/LICENSE` carries an explicit exception below the
MIT grant, and `characters/bufo/README.md` repeats it:

> The GIF assets in characters/bufo/ are from the community "bufo" emoji set
> (https://bufo.zone). They are third-party artwork redistributed for
> convenience and are not covered by the MIT license above; all rights remain
> with their original creators.

That is fifteen GIFs under `characters/bufo/`. **They are effectively
unlicensed for our purposes and must never be copied into this project**, in
any form: not as GIFs, not converted to sprites, not traced, not "inspired by".
The same caution applies to every other character pack anyone drops into that
project later.

Confirmed clean: `grep -rn bufo` over this repo, excluding `vendor/`, returns
three hits, all inside `docs/design-spec.md`, and all of them citations of
`characters/bufo/manifest.json`, which is a five-entry colour table in a JSON
file covered by the MIT grant, not artwork. No image file from that project
exists anywhere in this tree.

---

## 5. This project's own licence

**There is no `LICENSE` file at the repo root, and `helper/package.json`
declares `"license": "MIT"`.** That is a contradiction and it has to be fixed
before publication: a package that claims MIT with no licence text grants
nothing, and a repo with no licence file is "all rights reserved" by default,
which is not what a gift with a "download this one file" card on it can be.

The fix is one file. Add `LICENSE` at the root with the MIT text and the
copyright line, or change `package.json` to whatever is actually intended. Not
done here because choosing a licence is the owner's call, not an auditor's.

---

## 6. What is actually inside the shipped binary

This is the section that matters if a flashed board is handed to somebody,
which is the whole point of the project. `firmware.bin` statically links:

| Component | Version | Licence | Source |
|---|---|---|---|
| LovyanGFX | 1.2.28 | BSD-2-Clause (plus the BSD-3-Clause and MIT headers of its ancestors) | `lib_deps` in `firmware/platformio.ini` |
| ArduinoJson | 7.4.3 | MIT, Copyright 2014-2026 Benoit Blanchon | `lib_deps` in `firmware/platformio.ini`; licence read from `firmware/.pio/libdeps/tdisplays3/ArduinoJson/LICENSE.txt` |
| Arduino core for ESP32 | `framework-arduinoespressif32` 3.20017.241212 | **LGPL-2.1-or-later** | its own `package.json` declares `"license": "LGPL-2.1-or-later"`, and `cores/esp32/Arduino.h` carries the LGPL-2.1 header, Copyright 2005-2013 Arduino Team |
| ESP-IDF components pulled in by that core (including `Preferences`, `esp_mac`, `esp_random`) | as shipped with the core above | mixed, predominantly Apache-2.0 | same package |

### The notice that has to travel with the binary

BSD-2-Clause and MIT both require the notice, not the whole text, to accompany
a binary distribution in its documentation. This is that documentation:

> LovyanGFX: Copyright (c) 2020 lovyan03. Portions Copyright (c) 2020 Bodmer
> (TFT_eSPI), Copyright (c) 2012 Adafruit Industries (Adafruit_GFX), and
> Limor Fried / Ladyada, Adafruit Industries (Adafruit_ILI9341). Redistributed
> in binary form under the FreeBSD (BSD 2-Clause), BSD 3-Clause and MIT
> licences respectively. Full text: `vendor/LovyanGFX/license.txt` upstream at
> https://github.com/lovyan03/LovyanGFX
>
> ArduinoJson: Copyright (c) 2014-2026 Benoit Blanchon. MIT.
> https://github.com/bblanchon/ArduinoJson
>
> Arduino core for ESP32: Copyright (c) 2005-2013 Arduino Team and
> contributors. LGPL-2.1-or-later.
> https://github.com/espressif/arduino-esp32

A link to this file is enough to carry all of it, which is the cheapest way to
satisfy it on a printed card: one line pointing at the repo.

### The LGPL point, stated plainly and not resolved here

LGPL-2.1 section 6 governs distributing a work that links the library. For a
statically linked binary it wants either the object files needed to relink
against a modified library, or an equivalent arrangement, plus a copy of the
licence. Handing somebody a flashed ESP32 is a distribution.

What this project already has going for it, without anyone doing extra work:

- **The complete source of the linking work is published**, in this repo.
- **The build is pinned and reproducible**: `platform = espressif32@7.1.1` and
  two exact library versions, so anyone can rebuild against a modified Arduino
  core with `pio run` and reflash with `pio run -t upload`. That is the
  practical substance of the relink right.
- **The fleet image hash is recorded** at flashing time, so a recipient can
  dump their board and check it against a build they made themselves
  (`firmware/README.md`, "Fleet image hash").

What is missing is the explicit written offer and the shipped licence copy.
Adding a line to `docs/card.md` pointing at this file and at the repo would
close it. **Whether that is sufficient is a legal question and this document
does not answer it**; it records the facts so that somebody who wants to answer
it has them. Note also that essentially every ESP32 Arduino project ever handed
to a friend has this exact shape, which is context and not a defence.

---

## 7. Everything else

- `helper/` has **no dependencies at all**. `helper/package.json` has no
  `dependencies` key, `npm install` in that directory installs nothing, and the
  only requires in `helper/codex-companion.js` are four Node built-ins
  (`node:fs`, `node:path`, `node:os`, `node:child_process`). Nothing to
  licence.
- The test suite is `node:test` from the Node standard library. No framework.
- `docs/make-qr.sh` shells out to `qrencode` (GPL-2.0-or-later) as an external
  tool at authoring time. Invoking a program is not linking, its output is not
  a derivative work, and no part of it ships. Nothing to do.
- `firmware/tools/sim.py` optionally uses `pyserial` (BSD-3-Clause), also an
  external tool, also not shipped, and not installed on the build machine
  today.
- Fonts on the panel are LovyanGFX's own built-in bitmap fonts, covered by
  section 2. No font file was added.

## 8. How to re-check all of this

```sh
# the four clones, their commits, and their licence files
for d in vendor/*/; do
  echo "== $d"
  git -C "$d" log -1 --format='%H %cI'
  ls "$d" | grep -iE '^(licen[cs]e|notice|copying)'
done

# prove no unlicensed artwork leaked in
grep -rni bufo --exclude-dir=vendor --exclude-dir=.pio .

# prove vendor/ is not published
git ls-files | grep '^vendor/' ; echo "empty above means clean"

# what is actually linked into the binary
grep -n lib_deps -A3 firmware/platformio.ini
```
