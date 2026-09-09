# Prior art

What already exists, what this project took from it, and what it deliberately
did not. Surveyed 2026-09-07 against Codex CLI 0.153.4. Three longer research
documents were merged into this one; the transcript is gone and the findings are
here.

## What already exists

**The closest thing to this project.**
[SixSigmaEngineer/Claude-Status-Bar-Lilygo](https://github.com/SixSigmaEngineer/Claude-Status-Bar-Lilygo)
(MIT) is a LilyGo T-Display S3 Long fed over **USB CDC serial** by a Python
bridge tailing transcripts, with a compact one-JSON-line-per-second protocol.
Same board family, same transport, same shape. Its weakness is its state signal:
`waiting` is a 20 second timeout plus a "does the last line end in a question
mark" heuristic.

**Desk companions.**
[Sbr1z/ccdev](https://github.com/Sbr1z/ccdev) is the best-designed of them
(round touch LCD, BLE, **no licence, so read it and do not copy it**).
[op7418/m5-paper-buddy](https://github.com/op7418/m5-paper-buddy) is an e-ink
companion that independently chose USB serial over BLE to avoid the pairing
permission dance. [niclasvestlund-YT/vibepulse](https://github.com/niclasvestlund-YT/vibepulse)
is the only project found that implements answerable prompts for both Claude
Code and Codex. [EESheep/agent-desk](https://github.com/EESheep/agent-desk)
(Apache-2.0) has the most legitimate Codex state detection in the survey: it
spawns `codex app-server --stdio` and reads `waitingOnUserInput` as a
first-class flag. [kaosrmw-eng/Porthole](https://github.com/kaosrmw-eng/Porthole)
uses this exact board (T-Display-S3, 170x320 ST7789) for something else
entirely.

**Status lights.** [shilongWang1107/codex-status-light](https://github.com/shilongWang1107/codex-status-light)
is the original, a macOS menu-bar app tailing rollout JSONL, with a Windows C#
port and an unlicensed Electron fork.
[queenmarina795-star/CodexStatusLight](https://github.com/queenmarina795-star/CodexStatusLight)
is the one that gets a real approval signal: it registers actual Codex hooks and
maps `PermissionRequest` to its attention state.

**App-server clients.** [JaminZhou/codex-app-server-client](https://github.com/JaminZhou/codex-app-server-client)
(TypeScript, MIT) is the correct one and the closest to what a Node helper
wants. [lucianlamp/codex-monitor](https://github.com/lucianlamp/codex-monitor)
is the best reference for endpoint discovery.
[financialvice/codex-app-server-client](https://github.com/financialvice/codex-app-server-client)
(Swift) has the best written prose. Two others,
[luisjpf/codex-app-server-client-cli](https://github.com/luisjpf/codex-app-server-client-cli)
and [paras-anekantvad/codex-app-server-client-sdk](https://github.com/paras-anekantvad/codex-app-server-client-sdk),
send approval payloads that match no schema in the binary. Do not copy their
shapes.

## What we took

**Codex's first-party hooks are the state mechanism.** Verified against the
installed binary rather than inferred: it carries a `codex_hooks` crate, a
`hooks/list` RPC and a `/hooks` TUI command, reads `$CODEX_HOME/hooks.json`,
and keeps a trust ledger in `hooks.state`. The event names, with occurrence
counts in the binary: `Notification` 694, `SessionStart` 55, `PreToolUse` 54,
`PermissionRequest` 54, `PostToolUse` 41, `SubagentStart` 35, `SessionEnd` 32,
`SubagentStop` 30, `PreCompact` 29, `PostCompact` 29, `UserPromptSubmit` 18.
This replaced a 45 second stall heuristic, and it is the single most valuable
thing the survey produced.

**The `PermissionRequest` contract, read out of the binary's own validation
error strings.** A handler returns `hookSpecificOutput.decision.behavior` of
`allow`, `deny` or `ask`. It **rejects** `updatedInput`, `updatedPermissions`,
`continue:false`, `stopReason`, `suppressOutput` and `interrupt:true`. So there
is no "allow all" available from a `PermissionRequest` hook: allow, deny, or
fall back to the terminal. `PreToolUse` is richer and does accept `updatedInput`,
but only alongside `permissionDecision:"allow"`.

**There is no `PermissionResolved` event**, and that shaped the staleness
timers. `queenmarina795-star/CodexStatusLight` latches amber on
`PermissionRequest` and has nothing to clear it with, so a
resolved-then-quiet session stays amber forever, and its `GlobalStatusResolver`
has no staleness handling at all, so a crashed process pins the light. This
device clears the amber on whatever event happens next, and its own 30 second
and 5 minute timers are the recovery when nothing happens next. It is also the
reason the do-not-disturb sign is one press to leave: a mode with no obvious
exit is the worst failure a small device can have.

**ccdev's alerting discipline.** Key the UI off a question id rather than off
the status, so a heartbeat re-asserting `waiting` does not re-alert. Queue a new
question rather than replacing the one on screen, because a tap already in
flight can land on a prompt nobody read. Guard a freshly drawn screen for 400ms
(`ASK_TAP_GUARD_MS`) for the same reason. Sound only on `waiting`, because the
rest are worth a glance and not an interruption.

**Fail-open, always.** Several projects converge on this: every failure path in
a hook handler is "do nothing and exit 0", and a serial error should exit rather
than leave a fake-alive process. A broken desk toy must never break somebody's
work.

**Warn about a config that suppresses everything.** vibepulse checks at session
start for `approval_policy = "never"`, `approvals_reviewer = "auto_review"` or
`sandbox_mode = "danger-full-access"`, any of which means no permission prompt
ever reaches the device, so it looks like broken hardware.

## The one hardware finding worth the whole survey

The ESP32-S3's native USB serial needs the build flag `CDCOnBoot=cdc`, and:

> The default **256-byte** CDC RX buffer overflows during 50ms screen redraws
> and silently corrupts inbound JSON.

The fix is `Serial.setRxBufferSize(16384)` **before** `Serial.begin()`. From
`SixSigmaEngineer/Claude-Status-Bar-Lilygo`'s own `HOW_IT_WORKS.md`. This is
exactly the class of bug that eats a two day deadline: it does not fail, it
corrupts.

Two smaller ones. CH340 and CP2102 boards reset the ESP32 when the port is
opened and native USB CDC boards do not, so a tool that wants a reset has to
pulse DTR and RTS itself (which is what `verify_hello` in
`firmware/tools/flash-all.sh` does). And every project in the survey runs at
115200 baud.

## What we deliberately did not take

- **No WiFi, no BLE, no cloud relay.** Most of these projects use one of the
  three, and each brings provisioning, a pairing dialog, or a stranger's server
  in the trust story. USB serial sidesteps all three, and it sidesteps macOS
  Bluetooth permission entirely.
- **No byte budgets copied from BLE projects.** ccdev's 30 character question
  budget and cc_bridge's compact codes exist because of the BLE MTU. On a USB
  CDC line they buy nothing, and a truncated question is the worst safety
  property in the best project in the survey.
- **No fresh interpreter per hook event.** Several projects spawn a new Python
  process per event, at 30 to 80ms each. This helper is one Node file, which is
  the same idea done cheaper.
- **No approving or denying, from anything.** Several clients answer approvals
  from a monitoring connection. Two do it with payloads that match no schema.
  This device's helper declines to decide, twice over, on purpose. See
  `docs/architecture.md` section 6.
- **No app-server attach.** It was fully mapped and then not used. The framing
  is newline-delimited JSON over stdio with no `Content-Length` headers, but the
  control socket at `$CODEX_HOME/app-server-control/app-server-control.sock` is
  **WebSocket over a Unix socket** with a full RFC 6455 handshake, which is
  about 150 lines of dependency to write by hand (`codex app-server proxy --sock`
  converts it back to plain JSONL, which is the way in if this is ever wanted).
  `thread/status/changed` carries a `ThreadActiveFlag` of exactly
  `waitingOnApproval` or `waitingOnUserInput`, which is the cheapest possible
  signal for a device like this. Two traps if anyone returns to it: there is no
  `thread/subscribe` method (subscription is a side effect of `thread/resume` or
  `thread/start`), and **`thread/resume` on a thread that is not already in
  `thread/loaded/list` can fork it**. Four of six surveyed clients spawn their
  own server and therefore see an empty thread table forever. The hook path made
  all of this unnecessary.
- **The rollout file cannot express "waiting for approval."** Approval request
  types exist in the binary but appear in no local rollout file. That is why the
  rollout is read for numbers only, and why the state path had to be the hooks.
