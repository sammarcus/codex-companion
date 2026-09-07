# Prior art: codex-status-light and its ports

Read-only survey (no repo was cloned). Sources read via `gh api .../contents/PATH`.
Line numbers refer to the file as served at `HEAD` on the date below.

Surveyed 2026-09-07. Local verification done against Codex CLI `codex-cli 0.153.4`
(`/opt/homebrew/lib/node_modules/@openai/codex/node_modules/@openai/codex-darwin-arm64/vendor/aarch64-apple-darwin/bin/codex`).

| Repo | Platform | Detection mechanism | License |
| --- | --- | --- | --- |
| `shilongWang1107/codex-status-light` | macOS, Swift | tails rollout JSONL | MIT |
| `hannahhuyy/codex-status-light-windows` | Windows, C# | tails rollout JSONL (port of the above) | MIT + NOTICE |
| `whz1122/codex-status-light` | Windows, Electron + Python | reads `state_5.sqlite` + `logs_2.sqlite` tracing logs, plus JSONL | none declared |
| `queenmarina795-star/CodexStatusLight` | Windows, .NET WinForms | **Codex hooks** (`hooks.json`) over a named pipe | MIT |

The four projects use three genuinely different mechanisms. Only the fourth
gets a real approval signal.

---

## 1. How task state is detected

### 1a. shilongWang1107/codex-status-light (the macOS original)

Pure rollout-JSONL tailing. No app-server, no process inspection for state, no sqlite,
no hooks.

Watched roots, `Sources/main.swift:295-299`:

```
let home = fileManager.homeDirectoryForCurrentUser
roots = [
    home.appendingPathComponent(".codex/sessions", isDirectory: true),
    home.appendingPathComponent(".codex/archived_sessions", isDirectory: true)
]
```

Polling loop, `Sources/main.swift:309-321`: a `DispatchSourceTimer` at **0.15s**
repeating, re-running directory discovery every 6th tick (~0.9s).

File discovery, `Sources/main.swift:340-361`: recursive enumeration for `*.jsonl`,
skipping anything whose `contentModificationDate` is older than **14 days**
(`Sources/main.swift:341`).

Incremental read, `Sources/main.swift:363-406`: a per-path `FileCursor` byte offset,
`FileHandle.seek(toOffset:)`, capped at `maxBytesPerFilePerPass = 1_048_576`
(`Sources/main.swift:276`). File shrink is treated as truncation and resets the cursor
plus purges that file's state (`Sources/main.swift:377-380`). An unterminated record
larger than `maxIncompleteEventBytes = 4_194_304` (`Sources/main.swift:277`) is dropped
to bound memory.

Event parsing, `Sources/main.swift:87-180`. It reads only the outer `type`, the
`payload.type`, and `timestamp`:

- `event_msg` / `task_started` (`:100-110`): creates a `TurnRecord` keyed by
  `payload.turn_id`, marks it `active`, and records it as the file's current turn.
- `event_msg` / `task_complete` and `turn_aborted` (`:112-122`): marks the turn
  inactive and clears its pending calls.
- `event_msg` / `token_count` (`:124-135`): usage, see section 3.
- `response_item` / `function_call` and `custom_tool_call` (`:148-162`): if the call
  looks interactive, its `call_id` goes into the turn's `pendingInteractiveCalls`.
- `response_item` / `function_call_output` and `custom_tool_call_output` (`:164-175`):
  removes the matching `call_id` from the pending set.
- everything else just bumps `lastEventAt` on the current turn (`:211-217`).

Process inspection is used for **one** thing only, not for state: scoping which turns
count as live. `codexLaunchDate()` at `Sources/main.swift:428-441` asks
`NSRunningApplication.runningApplications(withBundleIdentifier:)` for
`com.openai.codex` / `com.openai.chat`, falling back to any running app whose
localized name contains "Codex", and takes the max `launchDate`. In `snapshot(...)`
(`Sources/main.swift:182-193`) a turn is eligible only if it started at or after
`launchDate - 8s`; when no Codex app is found (CLI-only case) it falls back to
"last event within 12 hours". That fallback is explicitly there to drop turns
abandoned by a crash (comment at `Sources/main.swift:190-191`).

### 1b. hannahhuyy/codex-status-light-windows

A direct C# transliteration of the Swift engine. Same roots
(`src/CodexStatusLight.cs:313`), same `task_started` / `token_count` handling
(`:104`, `:126`), same pending-call bookkeeping (`:149`), same
`window_minutes` / `used_percent` parsing (`:215-216`). The only behavioural
difference is one extra interactive tool name, see section 2.

### 1c. whz1122/codex-status-light

Reads Codex's **local sqlite state and tracing logs**, not just the rollout.
`scripts/collect_status.py:14-17`:

```
CODEX_HOME = Path.home() / ".codex"
LOG_DB = CODEX_HOME / "logs_2.sqlite"
STATE_DB = CODEX_HOME / "state_5.sqlite"
SESSIONS_ROOT = CODEX_HOME / "sessions"
```

- `state_5.sqlite`, table `threads`, columns `id, title, approval_mode, updated_at_ms, cwd`
  (`scripts/collect_status.py:101-107`). It picks the single thread whose normalized
  `cwd` matches a configured `targetCwd` (`:109-119`). So it is scoped to one project
  directory, not global.
- `logs_2.sqlite`, table `logs`, columns `id, ts, target, feedback_log_body`
  (`scripts/collect_status.py:130-139`). It greps the last 120 rows for the substrings
  `response.created`, `response.in_progress`, `response.output_text.delta`,
  `response.completed`, `decision=pending`, `decision=approved`, plus tracing targets
  `codex_core::spawn%` and `codex_core::stream_events_utils%`
  (`scripts/collect_status.py:151-164`).
- The rollout JSONL is used only for the human-readable detail line and the
  approval hint (`:174-179`, `:227-260`).

State machine, `scripts/collect_status.py:340-367`. Busy requires one of the streaming
markers **and** `stale_seconds <= 30` (`:360`), where staleness comes from the thread
row's `updated_at_ms` (`:337`).

Local verification: both databases exist on this machine and the schemas match.
`~/.codex/state_5.sqlite` has a `threads` table with `cwd`, `approval_mode`,
`updated_at_ms`; `~/.codex/logs_2.sqlite` has a `logs` table with
`ts, level, target, feedback_log_body, thread_id`. Caveat: the `logs` table here
currently holds 0 committed rows (content may be sitting in the `-wal`), so the
`decision=pending` string could not be observed directly. Treat that specific
marker as plausible but unconfirmed.

### 1d. queenmarina795-star/CodexStatusLight

Does not read the rollout at all. It registers itself as a set of **Codex hooks** and
receives push events.

`Program.cs:12-20` gives the binary a second personality: invoked as
`CodexStatusLight.exe hook <pipeName>`, it reads a JSON payload from stdin
(`Program.cs:29`), maps it to a status, and forwards it over a named pipe to the
running tray app (`Program.cs:33-36`). Failures are swallowed deliberately:
"Hook mode must never interrupt Codex" (`Program.cs:41`).

The stdin payload fields it reads, `Core/HookInputParser.cs:15-27`:
`session_id` (required), `hook_event_name` (required), `turn_id`, `cwd`.

Registration, `scripts/Install.ps1:12` and `:17`:

```
$events = @('PreToolUse', 'PostToolUse', 'PermissionRequest', 'UserPromptSubmit', 'Stop', 'SessionEnd')
$hooksPath = Join-Path ([IO.Path]::GetFullPath($CodexHome)) 'hooks.json'
```

It merges into an existing `hooks.json` non-destructively, backing it up first
(`scripts/Install.ps1:86-97`), and tags its own entries with a `statusMessage` marker
so uninstall can remove exactly them (`:53-74`). The README then instructs the user to
run `/hooks` in the Codex CLI and press `t` to trust each new hook.

Aggregation across concurrent sessions: `Core/SessionStatusStore.cs:8-20` keeps one
latest message per `session_id` (rejecting out-of-order timestamps at `:12`), and
`Core/GlobalStatusResolver.cs:5-22` folds them: any NeedsAttention wins immediately,
else any Working wins, else Stopped.

---

## 2. Can it detect "waiting for user approval"?

**Two different answers, and the difference is the whole finding.**

### The JSONL-based projects: no. They infer it, and the inference is weak.

`Sources/main.swift:247-267`, `isInteractiveCall`:

```
guard let name = payload["name"] as? String else { return false }

if ["request_user_input", "request_plugin_install"].contains(name) {
    return true
}

let execNames = ["exec_command", "exec"]
guard execNames.contains(name),
      let arguments = (payload["arguments"] as? String) ?? (payload["input"] as? String) else {
    return false
}
...
return decoded["sandbox_permissions"] as? String == "require_escalated"
```

with a substring fallback when the arguments string is not valid JSON
(`Sources/main.swift:263-264`).

The state rule is at `Sources/main.swift:202-208`: waiting if any eligible active turn
has a non-empty `pendingInteractiveCalls`, else running if any eligible turn is active,
else idle.

So the claimed "waiting for approval" is really: **a tool call was written to the
rollout, and its matching `function_call_output` has not been written yet, and the call
is one of a hardcoded list of names, or is an exec whose arguments carry
`sandbox_permissions: require_escalated`.**

That is a proxy, not a signal, and it has concrete failure modes:

- It fires the instant the escalated `function_call` is recorded, whether or not
  Codex is actually blocking on a human. The pending window covers approval wait
  **plus** command execution time, indistinguishably. A long escalated command that
  was auto-approved shows amber for its entire runtime.
- It cannot see approval prompts for calls that are not on the name list and do not
  carry `require_escalated`, that is, an ordinary command that trips the sandbox and
  triggers an approval prompt under an `on-request` policy.
- It has no notion of the decision. Approve and deny both end the pending window the
  same way, by the arrival of the output record.
- `request_user_input` and `request_plugin_install` are real interactive tools, so
  those two cases are sound. The exec/escalation case is the heuristic one.

The bundled self-test spells the model out. `Sources/main.swift:637-641` asserts a
pending `request_user_input` is waiting, and `Sources/main.swift:656-660` asserts a
`function_call` with `"sandbox_permissions":"require_escalated"` is waiting. Note the
test's own message at `Sources/main.swift:658` calls it "pending approval", which is
exactly the conflation described above.

`hannahhuyy/codex-status-light-windows` adds one name,
`src/CodexStatusLight.cs:236`:

```
if (name == "request_user_input" || name == "request_plugin_install" || name == "request_permissions") return true;
```

and, at `src/CodexStatusLight.cs:241`, loosens the check to a bare case-insensitive
substring search for `require_escalated` anywhere in the arguments before attempting
the JSON parse, which is strictly more false-positive-prone than the Swift original.

`whz1122` uses the same `sandbox_permissions == "require_escalated"` JSONL heuristic
(`scripts/collect_status.py:255`) but adds a second, independent signal: the string
`decision=pending` in the sqlite tracing log (`scripts/collect_status.py:158`, applied
at `:348`). It also gates on the thread's own `approval_mode != "never"`
(`scripts/collect_status.py:344`, `:348`), which is the one piece of real policy
awareness in any of the JSONL-based projects. `decision=pending` looks like a genuine
approval-state marker emitted by Codex's tracing, but see the caveat in section 1c:
it could not be observed in the local log database.

### The hooks-based project: yes, properly, via a first-class `PermissionRequest` hook.

`Core/HookStateMapper.cs:7-13` is the entire state machine:

```
message = input.EventName switch
{
    "UserPromptSubmit" or "PreToolUse" or "PostToolUse" => Create(StatusKind.Working),
    "PermissionRequest" => Create(StatusKind.NeedsAttention),
    "Stop" or "SessionEnd" => Create(StatusKind.Stopped),
    _ => null
};
```

`PermissionRequest` is a real Codex hook event, not an invention of that repo.
Verified directly against the installed Codex 0.153.4 native binary, which contains
the hook event enum:

```
PreToolUse PermissionRequest PostToolUse PreCompact PostCompact SessionStart
SessionEnd SubagentStart SubagentStop Stop Interrupt
```

and their snake_case wire forms (`pre_tool_use`, `permission_request`,
`post_tool_use`, ...), the config filename `hooks.json`, the payload key
`hook_event_name`, and dedicated serde types
`PermissionRequestCommandOutputWire`, `PermissionRequestHookSpecificOutputWire`,
`PermissionRequestDecisionWire` (with `behavior`, `updatedInput`,
`updatedPermissions`), alongside `permissionDecision` and
`permissionDecisionReason`.

The hook payload field set present in that binary is:
`session_id`, `turn_id`, `agent_type`, `transcript_path`, `agent_transcript_path`,
`cwd`, `hook_event_name`, `model`, `permission_mode`, `trigger`, `tool_name`,
`tool_input`, `tool_use_id`, `tool_response`, `prompt`, `last_assistant_message`,
`reason`, `stop_hook_active`, `agent_id`. Hook config entries support
`command`, `shell`, `if`, `matcher`, `timeout` / `timeoutSec`, `async`,
`asyncRewake`, and `statusMessage`, and there is a `disableAllHooks` switch.

**This is the mechanism you were missing.** The conclusion that the rollout JSONL
cannot express "waiting for approval" is correct, and the fix is not a better JSONL
parser: it is registering a `PermissionRequest` hook in `~/.codex/hooks.json`, which
Codex invokes synchronously at the moment it blocks on the human, handing you
`tool_name`, `tool_input`, `session_id`, `turn_id`, `cwd`, and `permission_mode`.

Supporting evidence that the rollout genuinely cannot carry this: the Codex binary
does define approval event types (`ExecApprovalRequest` / `exec_approval_request`,
`ApplyPatchApprovalRequest` / `apply_patch_approval_request`, `ExecCommandApproval`,
`PatchApproval`, `elicitation_request`), but none of them appear in any local rollout
file, and none of the four prior-art projects reads them from a rollout. A survey of
the local `~/.codex/sessions/**/*.jsonl` files shows only
`session_meta`, `turn_context`, `world_state`, `response_item/message`,
`event_msg/task_started`, `event_msg/task_complete`, `event_msg/item_completed`,
`event_msg/user_message`. Consistent with approval-request events being live protocol
events that the rollout recorder does not persist.

Practical caveats on the hook route, from the port's own README:

- The user must explicitly trust each hook: run `/hooks` in the Codex CLI and press
  `t`. Installation alone is not enough.
- Hooks are per-`CODEX_HOME` global config, so an installer must merge into an
  existing `hooks.json` rather than overwrite it, and should tag its own entries for
  clean removal (`scripts/Install.ps1:53-74`, `:86-97`).
- The hook process must never fail loudly. `Program.cs:39-42` swallows every
  exception, and the pipe send is given a 250 ms budget (`Program.cs:34`).
- There is no `PermissionResolved` counterpart in the event list. The port clears
  amber only when a later `PreToolUse` / `PostToolUse` / `Stop` arrives for that
  session (`Core/HookStateMapper.cs:9-11`), so the amber-to-green transition is
  edge-driven and a session that is approved but then goes quiet stays amber.

---

## 3. The 5-hour and 7-day usage display

### JSONL route (macOS original and the hannahhuyy port)

Source: the `event_msg` / `token_count` record's `payload.rate_limits` object.
`Sources/main.swift:124-134` reads it, `Sources/main.swift:227-239` parses it:

```
for key in ["primary", "secondary"] {
    guard let window = rateLimits[key] as? [String: Any],
          let minutes = number(window["window_minutes"]),
          Int(minutes) == targetMinutes,
          let usedPercent = number(window["used_percent"]) else {
        continue
    }
    let remaining = Int(max(0, min(100, 100 - usedPercent)).rounded())
    return UsageWindow(remainingPercent: remaining, updatedAt: updatedAt)
}
```

- The 5-hour window is selected by `window_minutes == 300` (`Sources/main.swift:126`).
- The 7-day window by `window_minutes == 10_080` (`Sources/main.swift:130`).
- It does **not** assume primary is 5h and secondary is 7d. It searches both slots for
  the matching duration, which is the right call.
- Displayed value is *remaining*, computed as `100 - used_percent`, clamped and
  rounded (`Sources/main.swift:235`).
- Per-file last-writer-wins by event timestamp (`Sources/main.swift:126-133`), then a
  global max-by-`updatedAt` across files at snapshot time
  (`Sources/main.swift:195-200`).

Self-test fixture, `Sources/main.swift:675`, shows the record shape:

```
{"type":"event_msg","payload":{"type":"token_count","rate_limits":{"primary":{"used_percent":23.0,"window_minutes":300,"resets_at":1784799278},"secondary":{"used_percent":41.0,"window_minutes":10080,"resets_at":1785399278}}}}
```

README limitation, confirmed in the README's "Limitations" section: usage only appears
after Codex writes a current `token_count` event. Before that, no number at all.

### App-server route (queenmarina795-star)

Does not touch the rollout. It spawns `codex app-server --stdio` and speaks JSON-RPC.
`Quota/CodexAppServerProcessClient.cs:20-28`:

```
{"id":1,"method":"initialize","params":{"clientInfo":{"name":"codex-status-light","version":"1.0"},"capabilities":{"experimentalApi":true}}}
{"method":"initialized"}
{"id":2,"method":"account/rateLimits/read"}
```

Process launch at `Quota/CodexAppServerProcessClient.cs:43-67`, 12-second timeout at
`:9`, and the child is killed with `entireProcessTree: true` after each read
(`:125-138`). So it is spawn-per-poll, not a persistent connection. Binary resolution
at `:69-85` prefers `$CODEX_STATUS_LIGHT_CODEX_COMMAND`, else `%APPDATA%\npm\codex.cmd`.

Response parsing, `Quota/CodexRateLimitParser.cs:32-54`: it looks first for
`rateLimitsByLimitId.codex` (falling back to the first object bucket), then for a
plain `rateLimits` object. Windows are read from `primary` and `secondary`
(`:20-21`) using **camelCase** field names, `usedPercent` (`:59`),
`windowDurationMins` (`:65`), `resetsAt` as unix seconds (`:72-75`).
Labels are chosen by duration at `:78-85`: 300 to "5 hour", 10080 to "weekly",
with generic day/hour fallbacks. Remaining is `1 - used/100` (`:86`).

Note the field-naming split: the rollout `token_count` payload uses
`used_percent` / `window_minutes` / `resets_at`, while the app-server RPC result uses
`usedPercent` / `windowDurationMins` / `resetsAt`. Both spellings are present in the
Codex 0.153.4 binary, along with `rateLimits`, `rateLimitsByLimitId`, `primary`,
`secondary`, `limit`, `used`, `remainingPercent` / `remaining_percent`, `hasCredits`,
and `account/rateLimits/read` (which appears in an error string,
"account/rateLimits/read failed during TUI refresh"). Both routes are real.

Failure policy, and it is a good one: `Quota/CodexAppServerQuotaProvider.cs:31-34`
returns `QuotaSnapshot.Unavailable` on any error rather than a stale or estimated
number, and the README states explicitly that it will never pass off an estimate or an
old cache as a real quota, and that a quota read failure does not change the
red/amber/green state.

---

## 4. States modelled, and the transition rules

### macOS original (and the hannahhuyy port)

Three states, `Sources/main.swift:13-16`: `running`, `waiting`, `idle`.
Rendered red, amber, green respectively (`Sources/main.swift:34-40`). Note the colour
mapping is inverted relative to the two Windows projects: here **red means running**
and green means idle.

Resolution order, `Sources/main.swift:202-208`, evaluated over eligible turns only:

1. any eligible turn has a non-empty pending interactive call set, then `waiting`
2. else any eligible turn exists, then `running`
3. else `idle`

Eligibility, `Sources/main.swift:183-193`: turn is `active`, and either
`startedAt >= codexLaunchDate - 8s` when a Codex app is running, or
`lastEventAt >= now - 12h` when it is not.

Transitions:

- `task_started` with a `turn_id`: create or replace the turn, `active = true`,
  becomes the file's current turn (`:100-110`).
- `task_complete` or `turn_aborted`: `active = false`, pending calls cleared
  (`:112-122`).
- interactive `function_call` / `custom_tool_call`: add `call_id` to pending
  (`:148-162`).
- matching `*_output`: remove `call_id` from pending (`:164-175`).
- any other event: only bumps `lastEventAt` (`:211-217`).
- file truncated or deleted: all of that file's turns are purged
  (`:76-85`, `:369-380`).

There is no explicit timeout out of `waiting`. Amber persists until the matching
output record lands, or the turn completes, or the turn falls out of eligibility.

### Electron/Python port

Three states, `scripts/collect_status.py:340-367`: `approval` (amber, flashing),
`busy` (green, breathing), `idle` (red, steady). Precedence: approval, then busy,
then idle. Busy additionally requires `stale_seconds <= 30` (`:360`), so it decays to
idle on its own after 30 seconds of no thread update, which the other projects do not
do. Both approval branches are gated on `thread.approval_mode != "never"`
(`:344`, `:348`).

### Hooks port

Three states, `Core/StatusKind.cs:3-8`: `Working`, `NeedsAttention`, `Stopped`,
rendered green, amber, red (README). Per-session latest message wins
(`Core/SessionStatusStore.cs:12-17`), and the global fold is
`NeedsAttention > Working > Stopped` (`Core/GlobalStatusResolver.cs:5-22`).
Purely edge-driven off hook events; there is no timeout and no polling of state.

---

## 5. License and reusability

- `shilongWang1107/codex-status-light`: **MIT**, "Copyright (c) 2026 Codex Status Light
  contributors". Reusable with attribution. The whole engine is one file,
  `Sources/main.swift`, 697 lines, and `StatusEngine` (`:64-268`) is cleanly separable
  from AppKit: it takes `process(line:file:)` and returns `snapshot(now:codexLaunchDate:)`,
  with no UI dependency. Directly liftable.
- `hannahhuyy/codex-status-light-windows`: **MIT**, plus a `NOTICE.md` attributing the
  macOS original and stating it is not affiliated with OpenAI. Reusable.
- `queenmarina795-star/CodexStatusLight`: **MIT**, "Copyright (c) 2026
  queenmarina795-star". Reusable. The interesting parts for a macOS project are not the
  C# but the two design decisions: the hook event to status mapping
  (`Core/HookStateMapper.cs`) and the safe `hooks.json` merge/backup/uninstall in
  `scripts/Install.ps1`. It also ships real unit tests, including
  `tests/CodexStatusLight.Tests/Core/HookStateMapperTests.cs` and
  `tests/CodexStatusLight.Tests/Quota/CodexRateLimitParserTests.cs`.
- `whz1122/codex-status-light`: **no license file at all**. `gh api` reports
  `license: null`. Default copyright applies, so it is **not** safe to copy code from.
  Read it for the technique (the sqlite tracing-log route and the `approval_mode` gate)
  and reimplement.

---

## 6. Gotchas and known-broken things

**Issue trackers are empty.** All four repos return zero issues for
`state=all`. There is no community bug list to mine. Stars: 3, 2, 2, 0. Treat all of
this as low-traffic hobby code, not battle-tested.

Called out by the projects themselves:

- macOS README, Limitations: "The event format is an internal Codex implementation
  detail and may change." Correct, and the sharpest risk in the whole approach.
- macOS README, Limitations: usage only appears after Codex writes a current
  `token_count` event. Cold start shows no numbers.
- macOS README: the release build is ad-hoc signed, not notarized. Requires
  right-click Open or a Privacy and Security approval.
- Windows hooks README, Known limitations: "Codex Hook or app-server interface changes
  may require adaptation"; no commercial code signing, so SmartScreen warns.
- Windows hooks README: the user must run `/hooks` and press `t` to trust each hook.
  A silent installer alone leaves the app dead.

Found by reading the code:

- **The amber state is a lie in the JSONL projects.** As detailed in section 2, the
  pending-call window conflates "blocked on a human" with "escalated command is
  running". Do not copy this and call it approval detection.
- **`codexLaunchDate` gating is fragile** (`Sources/main.swift:186-188`). If any
  running app happens to have "Codex" in its localized name (`:437-440`), the
  8-second-before-launch cutoff silently drops every turn started before that app
  launched. In a CLI-only workflow the intended path is the 12-hour fallback, but the
  name-substring match can steal it.
- **14-day discovery cutoff** (`Sources/main.swift:341`) is applied at discovery, so a
  session file older than 14 days that starts being written again is only picked up on
  the next discovery pass, which runs every ~0.9s, so in practice fine, but a resumed
  ancient session is invisible until its mtime updates.
- **Polling at 0.15s over a recursive directory enumeration** of
  `~/.codex/sessions` (`Sources/main.swift:310`, `:344-360`) is heavy for a menu bar
  app once the session tree is large. Discovery is throttled to every 6th tick, but
  `readChangedFiles()` stats every tracked file every 150 ms.
- **The substring fallback in `isInteractiveCall`** (`Sources/main.swift:263-264`) and
  the hannahhuyy version's unconditional substring test
  (`src/CodexStatusLight.cs:241`) will match `require_escalated` appearing anywhere in
  a command's arguments, including inside a string the model is merely quoting.
- **whz1122 is single-project scoped and hardcoded.** `DEFAULT_TARGET_CWD` at
  `scripts/collect_status.py:20` is a literal UNC path from the author's machine,
  `\\192.168.1.119\work\ai-web-react`. Its busy detection also greps for
  `model=gpt-5.4` and `reasoning_effort=high` (`scripts/collect_status.py:160-161`,
  `:358-359`), which are the author's own settings, not general signals. Both are
  brittle to the point of being non-portable.
- **whz1122 depends on undocumented sqlite filenames with version suffixes**,
  `logs_2.sqlite` and `state_5.sqlite`. Those numbers are migration generations and
  will change. Both files do exist on this machine at Codex 0.153.4, alongside
  `goals_1.sqlite`, `memories_1.sqlite`, `queue_1.sqlite`, and
  `thread_history_1.sqlite`.
- **The app-server quota read spawns and kills a full `codex app-server` process on
  every poll** (`Quota/CodexAppServerProcessClient.cs:15`, `:125-138`), with a 12-second
  timeout. That is expensive; consider the daemon at `~/.codex/app-server-daemon`
  or a persistent connection instead.
- **The hooks port has no way out of amber except a subsequent event**
  (`Core/HookStateMapper.cs:9-11`). No `PermissionResolved` event exists in the
  hook list, so a resolved-then-quiet session stays amber.
- **`GlobalStatusResolver` has no staleness handling** at all
  (`Core/GlobalStatusResolver.cs:5-22`). A crashed Codex process never emits
  `SessionEnd`, so its session sticks in the store as Working or NeedsAttention
  forever and pins the global light. The macOS project's 12-hour fallback exists
  precisely to handle this case, and the hooks port has no equivalent.

---

## Recommendation for codex-buddy

Combine the two mechanisms rather than picking one.

1. **Approval state: use a `PermissionRequest` hook in `~/.codex/hooks.json`.** It is
   a first-class, verified Codex feature in 0.153.4, it is the only source of truth,
   and no amount of rollout parsing substitutes for it. Merge into the existing
   `hooks.json` with a backup and a removable marker, and tell the user to run `/hooks`
   and trust it.
2. **Running/idle: `PreToolUse` / `PostToolUse` / `UserPromptSubmit` / `Stop` /
   `SessionEnd` hooks**, or the rollout's `task_started` / `task_complete` if you want
   to work without user-trusted hooks. Add a staleness timeout the hooks port lacks.
3. **Usage: prefer the rollout `token_count` `rate_limits` object**
   (`used_percent` / `window_minutes`), because it is free and already being tailed.
   Fall back to `account/rateLimits/read` over the app-server (`usedPercent` /
   `windowDurationMins`) when no `token_count` has been seen yet, which fixes the
   cold-start blank the macOS project documents as a limitation. Never estimate.
