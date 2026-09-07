# Codex CLI on-disk state format

How a host helper can derive agent state and metrics by watching `~/.codex/`.

**Version basis:** `codex-cli 0.153.4` (npm `@openai/codex@latest`, installed to an isolated
prefix, see "Install situation"). Source read from `openai/codex` at commit
`121f91fd5d9dc66017866ce9bdc49f1e182721df` (2026-09-07), cloned to
`/Users/sam/claude/codex-buddy/vendor/codex`.

**Empirical basis:** one real session captured on 2026-09-07 with 0.153.4, at
`~/.codex/sessions/2026/09/07/rollout-2026-09-07T06-20-00-01a07a18-2f55-78c3-9976-e71905ebb698.jsonl`,
copied to `helper/test/fixtures/session-sample.jsonl`. Its turn **failed on expired auth**, so it
contains the session/turn lifecycle lines but no model output, no token accounting, and no
approval traffic. Those event shapes are documented from source and hand-assembled into
`helper/test/fixtures/session-sample-synthetic.jsonl`, which is explicitly marked synthetic.

---

## 0. Headline findings

1. **Approval requests are NOT in the rollout JSONL.** `rollout/src/policy.rs` classifies
   `EventMsg::ExecApprovalRequest` and `EventMsg::ApplyPatchApprovalRequest` as "Transient,
   non-durable events" and returns `false` for them. A helper that only tails `~/.codex/sessions/`
   **cannot** see "waiting for approval". This is the single most important constraint on the design.
2. **There IS a programmatic approval channel.** The app-server exposes a Unix control socket and
   sends server-to-client JSON-RPC approval requests. Details in section 7. Confirmed yes.
3. **The app-server also exposes an explicit state machine** (`ThreadStatus` =
   `notLoaded | idle | systemError | active{activeFlags:[waitingOnApproval|waitingOnUserInput]}`)
   pushed as `thread/status/changed` notifications. That is exactly the busy/idle/waiting model,
   already computed by Codex, and it is not derivable from disk.
4. Rollout writes are **line-atomic for tailing**: every record is one
   `write_all(json + "\n")` followed immediately by `flush()`
   (`rollout/src/recorder.rs:1997-2001`). No partial lines under normal operation.
5. Recent rollouts use `history_mode: "paginated"`, which changes which events are persisted.
   The real capture confirms `"history_mode":"paginated"`.

---

## 1. Install situation (read this before trusting any version claim)

- `/opt/homebrew/bin/codex` is a Homebrew symlink to `../Cellar/codex/0.36.0/bin/codex`.
  `brew list --versions codex` reports `codex 0.36.0`. The backgrounded brew upgrade exited 0 but
  changed nothing; the formula is still 0.36.0.
- `npm i -g @openai/codex@latest` **fails** with `EEXIST: /opt/homebrew/bin/codex` because npm's
  global bin dir is `/opt/homebrew/bin` and the brew symlink already owns that name. It was NOT
  forced, and the brew install was left untouched.
- 0.153.4 was installed to an isolated prefix instead:
  `npm i -g --prefix <scratch>/npm-prefix @openai/codex@latest`, giving
  `<scratch>/npm-prefix/bin/codex` -> `codex-cli 0.153.4`. All 0.153.4 behavior below was produced
  with that binary.
- **The system `codex` on PATH is still 0.36.0.** A helper must not assume the on-PATH binary
  matches the format described here. Read `session_meta.payload.cli_version` from the rollout
  itself rather than shelling out to `codex --version`.

The format gap matters. A 0.36.0 rollout looks like this (real, from
`~/.codex/sessions/2025/09/16/...`):

```json
{"timestamp":"2025-09-16T19:49:09.591Z","type":"session_meta","payload":{"id":"38b1b318-65e0-465a-ad0c-9bebd99d1a93","timestamp":"2025-09-16T19:49:09.578Z","cwd":"/Users/sam","originator":"codex_cli_rs","cli_version":"0.36.0","instructions":null}}
```

No `ordinal`, no `session_id`, no `history_mode`, no `context_window`, no `source`.

---

## 2. File layout under `$CODEX_HOME` (default `~/.codex`)

| Path | What it is | Useful for state? |
|---|---|---|
| `sessions/YYYY/MM/DD/rollout-YYYY-MM-DDThh-mm-ss-<uuid>.jsonl` | The rollout / session transcript. One JSON object per line. **This is the primary source.** | Yes, primary |
| `archived_sessions/` | Same shape, archived. Constant `ARCHIVED_SESSIONS_SUBDIR`, `rollout/src/recorder.rs:1457` | No |
| `app-server-control/app-server-control.sock` | Unix control socket for the app-server daemon | Yes, for live state + approvals |
| `app-server-control/app-server-startup.lock` | Daemon startup lock | Liveness hint only |
| `config.toml` | User config: `model`, `tool_output_token_limit`, `[mcp_servers.*]` | Static config only |
| `auth.json` | Credentials. **Not read.** `ls -la` only: `-rw------- 1 sam staff 4247 Sep 16 2025` | No |
| `version.json` | `{"latest_version":"0.36.0","last_checked_at":"2025-09-16T19:47:13.051655Z"}`. Stale update-check cache, **not the installed version** | No, misleading |
| `internal_storage.json` | `{"gpt_5_codex_model_prompt_seen": true}`. UI dismissal flags | No |
| `.codex-global-state.json` | Electron desktop-app window bounds and workspace roots | No |
| `sqlite/codex-dev.db` | Desktop app only. Tables: `inbox_items`, `automations`, `automation_runs`. **No session, turn, token, or approval state.** | No |
| `log/codex-tui.log` | TUI tracing log, only written by the interactive TUI | Weak fallback |
| `tmp/arg0/codex-arg0*/` | execve-wrapper shim dir | No |
| `memories/`, `skills/`, `vendor_imports/` | Content, not state | No |

Filename format is confirmed by `rollout/src/list.rs:436` and
`rollout/src/rollout_file_name_tests.rs:18`. A reverted thread gets a second UUID appended:
`rollout-<ts>-<thread_id>_<rollout_id>.jsonl` (`rollout_file_name_tests.rs:22`), so **do not assume
the UUID in the filename is the thread id** for reverted threads. Read `payload.id` instead.

`--ephemeral` on `codex exec` skips persistence entirely: no rollout file is written. A helper must
treat "no file" as a real possibility, not an error.

---

## 3. Line envelope

`RolloutLine`, `history/src/lib.rs:254`:

```rust
pub struct RolloutLine {
    pub timestamp: String,                 // "[year]-[month]-[day]T[hh]:[mm]:[ss].[3-digit]Z", UTC
    pub ordinal: Option<u64>,              // monotonic per rollout file; absent in pre-paginated files
    #[serde(flatten)] pub item: RolloutItem,
}
```

`RolloutItem` is `#[serde(tag = "type", rename_all = "snake_case")]` with a `payload` field
(`history/src/rollout_payload.rs:22-24`). So every line is:

```
{"timestamp": ..., "ordinal": N, "type": "<snake_case_kind>", "payload": {...}}
```

`type` is one of: `session_meta`, `response_item`, `inter_agent_communication`,
`inter_agent_communication_metadata`, `compacted`, `turn_context`, `token_usage_record`,
`world_state`, `retained_context`, `security_risk_score`, `event_msg`, `realtime_item`.

For `type: "event_msg"`, `payload` is itself tagged: `EventMsg` is
`#[serde(tag = "type", rename_all = "snake_case")]` (`protocol/src/protocol.rs:1353`), so
`payload.type` is the event kind.

**Two renames that will bite you** (`protocol/src/protocol.rs:1405,1414`):

```rust
#[serde(rename = "task_started", alias = "turn_started")]  TurnStarted(TurnStartedEvent),
#[serde(rename = "task_complete", alias = "turn_complete")] TurnComplete(TurnCompleteEvent),
```

On the wire it is **`task_started` / `task_complete`**, not `turn_started` / `turn_complete`.
Match both to be safe.

---

## 4. What actually lands in the JSONL

Authoritative source: `rollout/src/policy.rs`, function `is_persisted_rollout_item` and
`should_persist_event_msg`.

**Always persisted (item kinds):** `session_meta`, `turn_context`, `token_usage_record`,
`world_state`, `retained_context`, `security_risk_score`, `compacted`,
`inter_agent_communication`, `inter_agent_communication_metadata`.
`response_item` is persisted for most variants (`should_persist_response_item`); dropped for
`AdditionalTools`, `CompactionTrigger`, `Other`.
`realtime_item` only in `paginated` history mode.

**`event_msg` persisted unconditionally:** `token_count`, `thread_goal_updated`,
`thread_rolled_back`, `turn_aborted`, `task_started`, `task_complete`, `thread_settings_applied`.

**`event_msg` persisted only in `paginated` mode:** `item_completed` (all `TurnItem` kinds).
In `legacy` mode only `FunctionCallOutput`, `Plan`, `Extension(Sleep)` and
`SubAgentActivity(Completed)` items survive.

**`event_msg` persisted only in `legacy` mode:** `user_message`, `agent_message`,
`agent_reasoning`, `agent_reasoning_raw_content`, `entered_review_mode`, `exited_review_mode`,
`patch_apply_end`, `context_compacted`, `mcp_tool_call_end`, `web_search_end`,
`image_generation_end`.

**NEVER persisted (explicitly "Transient, non-durable events"):**
`exec_approval_request`, `apply_patch_approval_request`, `request_permissions`,
`request_user_input`, `elicitation_request`, `guardian_assessment`,
`exec_command_begin`, `exec_command_end`, `exec_command_output_delta`,
`patch_apply_begin`, `patch_apply_updated`, `turn_diff`, `item_started`,
`error`, `stream_error`, `session_configured`, `plan_update`, `shutdown_complete`,
all `*_delta` streaming events, `mcp_tool_call_begin`, `web_search_begin`,
`hook_started`, `hook_completed`, `thread_queue_changed`, and the whole `collab_*_begin/end` set.

Consequences for the helper:

- You can see a turn start and end, but **not** individual command starts/ends.
- You can see completed items, but **not** in-progress ones (`item_started` is dropped).
- You **cannot** see approval requests, permission requests, or elicitations.
- You **cannot** see errors as events; a terminal error only appears inside `task_complete.error`.

---

## 5. Exact JSON paths

Root of every line is the envelope in section 3. Paths below are relative to the line object.

### Session identity

| Datum | Path | Notes |
|---|---|---|
| Session id | `payload.session_id` on `type=="session_meta"` | Equals the root thread id. **Absent before ~0.15x**; the deserializer back-fills it from `id` (`protocol.rs:3150-3162`), so a reader should do the same |
| Thread id | `payload.id` on `session_meta` | The stable id across revert. Prefer this over the filename UUID |
| Forked from | `payload.forked_from_id`, `payload.forked_from_ordinal_exclusive` | Omitted when absent |
| Parent thread | `payload.parent_thread_id` | Sub-agents |
| cwd | `payload.cwd` on `session_meta` | Also per-turn at `payload.cwd` on `turn_context` |
| Originator | `payload.originator` | Observed `"codex_exec"` (0.153.4 exec), `"codex_cli_rs"` (0.36.0) |
| CLI version | `payload.cli_version` on `session_meta` | **Use this, not `codex --version`** |
| Source | `payload.source` | Observed `"exec"` |
| Thread source | `payload.thread_source` | Observed `"user"` |
| Model provider | `payload.model_provider` | Observed `"openai"` |
| History mode | `payload.history_mode` | `"paginated"` or `"legacy"`. **Gates which events you will see** |
| Context window id | `payload.context_window.window_id` | UUIDv7 identity of the context window |
| Git info | `payload.git` on `session_meta` | Optional `GitInfo` |

**`model` is NOT in `session_meta`.** It lives on `turn_context`.

Real line (truncated at `base_instructions`, which is ~20 KB):

```json
{"timestamp":"2026-09-07T04:20:00.640Z","ordinal":0,"type":"session_meta","payload":{"session_id":"01a07a18-2f55-78c3-9976-e71905ebb698","id":"01a07a18-2f55-78c3-9976-e71905ebb698","timestamp":"2026-09-07T04:20:00.469Z","cwd":"/private/tmp/.../codex-run","originator":"codex_exec","cli_version":"0.153.4","source":"exec","thread_source":"user","model_provider":"openai","base_instructions":{"text":"You are a coding agent running in the Codex CLI...","provenance":{"type":"model","model":"gpt-5-codex"}},"history_mode":"paginated","context_window":{"window_id":"01a07a18-2f55-78c3-9976-e7245db69846"}}}
```

Note the first line is ~21 KB because of `base_instructions`. A tailer should not assume small lines.

### Model and policy (per turn)

Path root: `type=="turn_context"`.

| Datum | Path |
|---|---|
| Model | `payload.model` |
| Turn id | `payload.turn_id`, `payload.root_turn_id` |
| cwd | `payload.cwd`, `payload.workspace_roots[]` |
| Approval policy | `payload.approval_policy` (`"never"`, `"on-request"`, ...) |
| Sandbox | `payload.sandbox_policy.type` (`"workspace-write"`, ...), `.network_access` |
| Permission profile | `payload.permission_profile`, `payload.file_system_sandbox_policy` |
| Personality | `payload.personality` |
| Collaboration mode | `payload.collaboration_mode.mode`, `.settings.model`, `.settings.reasoning_effort` |
| Reasoning effort | `payload.reasoning_effort` (omitted when null) |
| Date / tz | `payload.current_date`, `payload.timezone` |

Real (abridged):

```json
{"timestamp":"2026-09-07T04:20:05.503Z","ordinal":5,"type":"turn_context","payload":{"turn_id":"01a07a18-2ffa-75a1-982a-dc92eb4b80f6","root_turn_id":"01a07a18-2ffa-75a1-982a-dc92eb4b80f6","cwd":"/private/tmp/.../codex-run","workspace_roots":["/private/tmp/.../codex-run"],"current_date":"2026-09-07","timezone":"Europe/Berlin","approval_policy":"never","approvals_reviewer":"user","sandbox_policy":{"type":"workspace-write","network_access":false,"exclude_tmpdir_env_var":false,"exclude_slash_tmp":false},"model":"gpt-5-codex","personality":"pragmatic","collaboration_mode":{"mode":"default","settings":{"model":"gpt-5-codex","reasoning_effort":null,"developer_instructions":null}},"multi_agent_version":"v1","realtime_active":false,"summary":"auto"}}
```

`turn_context` is emitted **more than once per turn**. In the 0.36.0-era capture six identical
`turn_context` lines appeared back to back. Treat it as "latest wins", not as a turn counter.

### Turn start

`type=="event_msg"`, `payload.type=="task_started"` (alias `turn_started`).
Struct `TurnStartedEvent`, `protocol.rs:2167`.

| Datum | Path |
|---|---|
| Turn id | `payload.turn_id` |
| Start time | `payload.started_at` (**Unix seconds**, optional) |
| **Model context window** | `payload.model_context_window` (i64, optional) |
| Collab mode | `payload.collaboration_mode_kind` |
| Trace id | `payload.trace_id` (optional) |

Real:

```json
{"timestamp":"2026-09-07T04:20:00.641Z","ordinal":1,"type":"event_msg","payload":{"type":"task_started","turn_id":"01a07a18-2ffa-75a1-982a-dc92eb4b80f6","started_at":1788754800,"model_context_window":258400,"collaboration_mode_kind":"default"}}
```

`model_context_window: 258400` for `gpt-5-codex` here. **This is the earliest and most reliable
place to read the context window size**: it appears on line 1, before any token accounting.

### Turn complete

`payload.type=="task_complete"` (alias `turn_complete`). Struct `TurnCompleteEvent`,
`protocol.rs:2141`.

| Datum | Path |
|---|---|
| Turn id | `payload.turn_id` |
| **Last assistant message** | `payload.last_agent_message` (String or null) |
| Terminal error | `payload.error` (`{message, codex_error_info}`), omitted on success |
| Start / end | `payload.started_at`, `payload.completed_at` (Unix seconds) |
| Elapsed | `payload.duration_ms` |
| TTFT | `payload.time_to_first_token_ms` (optional) |

Real (this one failed):

```json
{"timestamp":"2026-09-07T04:20:07.491Z","ordinal":8,"type":"event_msg","payload":{"type":"task_complete","turn_id":"01a07a18-2ffa-75a1-982a-dc92eb4b80f6","last_agent_message":null,"error":{"message":"Your access token could not be refreshed because your refresh token was already used. Please log out and sign in again.","codex_error_info":"unauthorized"},"started_at":1788754800,"completed_at":1788754807,"duration_ms":6854}}
```

**`last_agent_message` on `task_complete` is the last-assistant-message marker.** It is the
canonical one, present in both history modes. In `paginated` mode you can also read the last
`item_completed` whose `payload.item.type == "AgentMessage"`.

### Turn aborted

`payload.type=="turn_aborted"`. Struct `TurnAbortedEvent`, `protocol.rs:4154`.
`payload.reason` is one of `interrupted`, `replaced`, `review_ended`, `budget_limited`.
Also carries `turn_id`, `started_at`, `completed_at`, `duration_ms`.

### Items

`payload.type=="item_completed"`. Persisted for all item kinds only in `paginated` mode.

| Datum | Path |
|---|---|
| Thread / turn | `payload.thread_id`, `payload.turn_id` |
| Item kind | `payload.item.type` |
| Item id | `payload.item.id` |
| Timing | `payload.started_at_ms`, `payload.completed_at_ms` (**milliseconds**, unlike turn events) |

`payload.item.type` uses **PascalCase** variant names (confirmed empirically, `TurnItem` has no
`rename_all`): `UserMessage`, `AgentMessage`, `Reasoning`, `CommandExecution`, `FileChange`,
`FunctionCallOutput`, `Plan`, `McpToolCall`, `WebSearch`, `ImageView`, `ImageGeneration`,
`DynamicToolCall`, `CollabAgentToolCall`, `SubAgentActivity`, `HookPrompt`, `Extension`,
`ContextCompaction`, `EnteredReviewMode`, `ExitedReviewMode` (`protocol/src/items.rs:45-77`).

Real:

```json
{"timestamp":"2026-09-07T04:20:05.818Z","ordinal":7,"type":"event_msg","payload":{"type":"item_completed","thread_id":"01a07a18-2f55-78c3-9976-e71905ebb698","turn_id":"01a07a18-2ffa-75a1-982a-dc92eb4b80f6","item":{"type":"UserMessage","id":"01a07a18-443a-7ab1-bd43-9154fec1aec0","content":[{"type":"text","text":"create a file hello.txt containing hi, then run ls","text_elements":[]}]},"started_at_ms":1788754805818,"completed_at_ms":1788754805818}}
```

### Token counts

Two independent sources.

**(a) `type=="token_usage_record"`**, from `TokenUsageRecord`, `protocol.rs:2239`. Written once per
upstream Responses API completion that reported usage.

| Datum | Path |
|---|---|
| Ids | `payload.thread_id`, `payload.turn_id`, `payload.session_id`, `payload.root_turn_id`, `payload.response_id` |
| **This response only** | `payload.usage` |
| **Cumulative in turn** | `payload.turn_token_usage` |
| **Cumulative in thread** | `payload.thread_token_usage` |

Semantics confirmed by `core/tests/suite/token_usage_rollout.rs:89-105`, which asserts the sequence
`(response-a, turn 120, thread 120) -> (response-b, turn 200, thread 200) -> (response-c, turn 30,
thread 230)`. Note `turn_token_usage` **resets** at a turn boundary while `thread_token_usage`
keeps climbing.

Each `TokenUsage` object (`protocol.rs:2216`) has exactly these numeric fields:

```
input_tokens, cached_input_tokens, cache_write_input_tokens,
output_tokens, reasoning_output_tokens, total_tokens
```

(`codex_rollout_budget_units` exists on the struct but is `#[serde(skip_serializing)]`, so it will
never appear in the JSONL.)

**(b) `type=="event_msg"`, `payload.type=="token_count"`**, from `TokenCountEvent`, `protocol.rs:2318`.
This is the **only place rate limits are persisted**.

| Datum | Path |
|---|---|
| Cumulative | `payload.info.total_token_usage` (a `TokenUsage`) |
| Last turn | `payload.info.last_token_usage` (a `TokenUsage`) |
| **Context window size** | `payload.info.model_context_window` (i64 or null) |
| Rate limits | `payload.rate_limits` (nullable) |

`payload.info` itself is nullable. The source notes that an absent value means unknown, and that UIs should not display anything when it is `None`.

### Model context window size

Three paths, in order of preference:

1. `event_msg` / `task_started` -> `payload.model_context_window`. Earliest, line 1 of the turn.
2. `event_msg` / `token_count` -> `payload.info.model_context_window`.
3. Not otherwise on disk. `session_meta.context_window` is `{"window_id": "<uuid>"}`, a UUIDv7
   **identity**, not a size (`SessionContextWindow`, `protocol.rs:3007`). Do not confuse them.

### Rate limits

Path root: `event_msg` / `token_count` -> `payload.rate_limits` (`RateLimitSnapshot`,
`protocol.rs:2324`).

| Datum | Path |
|---|---|
| Limit identity | `payload.rate_limits.limit_id`, `.limit_name` |
| **Primary window %** | `payload.rate_limits.primary.used_percent` (f64, 0-100) |
| Primary window length | `payload.rate_limits.primary.window_minutes` (minutes) |
| **Primary reset** | `payload.rate_limits.primary.resets_at` (**Unix seconds**) |
| **Secondary window %** | `payload.rate_limits.secondary.used_percent` |
| Secondary window length | `payload.rate_limits.secondary.window_minutes` |
| **Secondary reset** | `payload.rate_limits.secondary.resets_at` |
| Credits | `payload.rate_limits.credits.{has_credits,unlimited,balance}` |
| Spend control | `payload.rate_limits.individual_limit.{limit,used,remaining_percent,resets_at}` |
| Reached flag | `payload.rate_limits.spend_control_reached` (bool or null) |
| Reached kind | `payload.rate_limits.rate_limit_reached_type` |
| Plan | `payload.rate_limits.plan_type` |

`primary` and `secondary` are each `Option<RateLimitWindow>` so they can be `null`.
`rate_limit_reached_type` is snake_case: `rate_limit_reached`,
`workspace_owner_credits_depleted`, `workspace_member_credits_depleted`,
`workspace_owner_usage_limit_reached`, `workspace_member_usage_limit_reached`.

### Approval requests

**These are not in the rollout JSONL.** See section 0 item 1 and section 4.

Payload shapes, for parsing off the app-server stream
(`protocol/src/approvals.rs:262` and `:449`):

`exec_approval_request`: `kind` (`command` | `write_stdin`), `call_id`, `approval_id` (optional;
present for subcommand/stdin approvals, absent when the approval is for `call_id` itself),
`turn_id`, `started_at_ms`, `command` (`Vec<String>`), `cwd`, `reason` (optional), `parsed_cmd`,
plus optional `plugin_id`, `script_path`, `environmentId`, `network_approval_context`,
`proposed_execpolicy_amendment`, `proposed_network_policy_amendments`,
`additional_permissions`, `available_decisions`.

`apply_patch_approval_request`: `call_id`, `turn_id`, `started_at_ms`,
`changes` (map of path -> `FileChange`), `reason` (optional), `grant_root` (optional).

### Other durable items worth knowing

- `type=="compacted"`: context compaction happened. `payload.message`,
  `payload.window_number`, `payload.window_id`, `payload.previous_window_id`, and
  `payload.latest_token_usage_record` (a full `TokenUsageRecord` snapshot, so token totals survive
  compaction without rescanning).
- `type=="world_state"`: `payload.full` (bool: snapshot vs patch) and `payload.state`, a big map
  including `state.model`, `state.environments.environments.local.{cwd,status,shell}`,
  `state.personality`, `state.permissions.approved_command_prefixes`. In the real capture this line
  was ~20 KB.
- `type=="security_risk_score"`, `type=="retained_context"`, `type=="inter_agent_communication"`.

---

## 6. Recommended state-derivation rule

### If you can only read `~/.codex/sessions/` (disk-only)

Watch the newest `rollout-*.jsonl` under `~/.codex/sessions/YYYY/MM/DD/`. Because every record is
flushed on write, a naive line-tail is safe.

Maintain per-file: `last_line_mtime`, `open_turn_id`, `last_event`.

```
state = DONE            # no rollout activity and no live process
if last event is task_started            -> BUSY (turn_id = payload.turn_id)
if last event is task_complete           -> IDLE if the process is still alive, else DONE
if last event is turn_aborted            -> IDLE (reason in payload.reason)
if last event is anything else AND an unmatched task_started precedes it -> BUSY
if BUSY and now - last_line_mtime > STALL_S  -> UNKNOWN_STALLED
```

`STALL_S` should be generous (60-120 s). Long model turns and long tool calls emit **nothing** to
the JSONL, because `exec_command_begin/end`, `item_started`, and all delta events are non-durable.
Silence is not evidence of being stuck.

**WAITING_FOR_APPROVAL is not derivable this way.** The honest disk-only mapping is:

```
BUSY + stalled + turn_context.approval_policy != "never"  ->  MAYBE_WAITING (heuristic only)
```

Label it a heuristic in the UI. Do not present it as a fact. With `approval_policy == "never"`
(what `codex exec` uses by default, confirmed in the real capture) approvals cannot occur at all,
so the heuristic can be suppressed entirely.

Liveness: pair the rollout tail with a process check for a `codex` process whose open files include
that rollout path, or simply whether the `codex exec` child you spawned has exited. `task_complete`
means "this turn ended", not "the session ended". An interactive session goes back to IDLE.

### Metrics (disk-only)

- **Context window fill %.** Take `W` from the most recent `task_started.model_context_window`
  (fall back to `token_count.info.model_context_window`). Take `used` from the most recent
  `token_count.info.last_token_usage.total_tokens`, or if no `token_count` yet, from the most
  recent `token_usage_record.payload.usage.total_tokens`.

  **Do not use the `total_*` / `thread_*` figures here.** `total_token_usage`,
  `thread_token_usage` and `turn_token_usage` are cumulative session accounting: they only ever
  grow, they count every turn's input and output added together, and feeding them into the
  formula below drives the ring to a static 100% within a few turns and pins it there.
  `tui/src/token_usage.rs:37-38` is explicit about the distinction: "For `last_token_usage`,
  this is the latest active context size; for `total_token_usage`, this is the accumulated
  session total." `usage` on a `token_usage_record` is the per-response analogue of
  `last_token_usage` (`protocol.rs:2237-2247`), which is why it is the fallback rather than
  `thread_token_usage`. `helper/src/codex-watcher.js` keeps the two in separate fields for
  exactly this reason: `contextTokens` (fed from `last_token_usage` / `usage`) is what reaches
  `contextFill()` and the ring, `totalTokens` (fed from the cumulative figures) is display-only
  accounting and never touches the ring.

  Codex's own formula is **not** `used/W`. From `TokenUsage::percent_of_context_window_remaining`
  (`protocol.rs:2428`) with `BASELINE_TOKENS = 12000` (`protocol.rs:2394`):

  ```
  if W <= 12000: remaining_pct = 0
  effective = W - 12000
  used_eff  = max(total_tokens - 12000, 0)
  remaining_pct = round(clamp((effective - used_eff) / effective, 0, 1) * 100)
  fill_pct = 100 - remaining_pct
  ```

  Match this exactly if you want your number to agree with what the Codex TUI shows.

- **Elapsed.** Prefer `task_complete.duration_ms` when the turn ended. While a turn is running:
  `now - task_started.started_at` (seconds). Do not mix units: turn events use **seconds**,
  item events use **milliseconds**.

- **Tokens/sec.** Codex does not report a rate. Compute it:
  `payload.usage.output_tokens` from a `token_usage_record`, divided by the wall time between
  that record and the previous one (use the envelope `timestamp`, which is ms-precision UTC).
  Turn-level: there is **no** token usage on the turn-complete event, so take the delta yourself:
  `(cumulative output_tokens at turn end - cumulative output_tokens at turn start) /
  (duration_ms / 1000)`, snapshotting the running `output_tokens` when `task_started` fires and
  reading it again at `task_complete`. `TurnCompleteEvent` (`protocol.rs:2141-2164`) carries
  exactly `turn_id`, `last_agent_message`, `error`, `started_at`, `completed_at`, `duration_ms`
  and `time_to_first_token_ms`, and nothing else: `turn_token_usage` lives on
  `token_usage_record` (section 5), not here. This is what `helper/src/codex-watcher.js` ships
  (`_turnStartTokensOut`, set in the `task_started` branch, differenced in the
  `TURN_COMPLETE` branch).
  Label it derived. It is an average over the interval, not an instantaneous rate, and it excludes
  time spent in tool calls.

- **Cached vs fresh input.** `input_tokens` **includes** `cached_input_tokens`. Codex computes
  `non_cached_input = max(input_tokens - cached_input_tokens, 0)` and
  `blended_total = non_cached_input + max(output_tokens, 0)` (`protocol.rs:2415-2426`). Use
  `blended_total` if you want a single "work done" number.

- **Rate limits.** Read the most recent `token_count.payload.rate_limits`. It only updates when a
  `token_count` event fires, so it can be stale between turns.

### If you can reach the app-server (recommended)

Do not derive the state machine. Ask for it. Subscribe to `thread/status/changed` and read
`ThreadStatus` directly:

| Codex `ThreadStatus` | Helper state |
|---|---|
| `{"type":"idle"}` | IDLE |
| `{"type":"active","activeFlags":[]}` | BUSY |
| `{"type":"active","activeFlags":["waitingOnApproval"]}` | WAITING_FOR_APPROVAL |
| `{"type":"active","activeFlags":["waitingOnUserInput"]}` | WAITING_FOR_INPUT |
| `{"type":"notLoaded"}` | DONE / not resident |
| `{"type":"systemError"}` | ERROR |

Plus `turn/started`, `turn/completed`, `thread/tokenUsage/updated` notifications, and the
`item/*/requestApproval` server requests. This is strictly better than the disk heuristic and is
the only correct way to get "waiting".

**Suggested design:** app-server as the primary source, rollout tail as the fallback and as the
durable audit trail. They agree on turn boundaries because both derive from the same `EventMsg`.

---

## 7. Programmatic approval channel: CONFIRMED YES

Three independent confirmations, all from the 0.153.4 binary or its source.

**1. There is a Unix control socket.**
`codex app-server --help` lists `proxy  Proxy stdio bytes to the running app-server control socket`.
Path is built by `app_server_control_socket_path`
(`app-server-transport/src/transport/mod.rs:59-65`) as
`$CODEX_HOME/app-server-control/app-server-control.sock`, from constants at `mod.rs:55-57`:

```rust
const APP_SERVER_CONTROL_SOCKET_DIR_NAME:  &str = "app-server-control";
const APP_SERVER_CONTROL_SOCKET_FILE_NAME: &str = "app-server-control.sock";
const APP_SERVER_STARTUP_LOCK_FILE_NAME:   &str = "app-server-startup.lock";
```

Transports supported: `AppServerTransport::{Stdio, UnixSocket{socket_path}, WebSocket{bind_address}, Off}`
(`mod.rs:76-81`). `codex agents --remote <ADDR>` accepts `ws://`, `wss://`, `unix://`, `unix://PATH`.

The daemon is managed with `codex app-server daemon {start,restart,stop,bootstrap,version,
enable-remote-control,disable-remote-control}`. **The socket does not exist on this machine right
now** (`~/.codex/app-server-control/` is absent), so an external process would need to start the
daemon first.

**2. Approval requests are server-to-client JSON-RPC requests you can answer.**
From the `ServerRequest` table in `app-server-protocol/src/protocol/common.rs`:

```
item/commandExecution/requestApproval   (line 1728)
item/fileChange/requestApproval         (line 1735)
item/permissions/requestApproval        (line 1753)
item/tool/requestUserInput              (line 1741)
mcpServer/elicitation/request           (line 1747)
```

Plus the deprecated v1 pair, still live for legacy turn APIs (`common.rs:1785-1794`), whose wire
method names are `applyPatchApproval` and `execCommandApproval` (asserted verbatim in the test at
`common.rs:2869`). Their params (`v1.rs:139-178`) are `{conversationId, callId, approvalId?,
command, cwd, reason, parsedCmd}` and `{conversationId, callId, fileChanges, reason, grantRoot}`;
both responses are `{decision: ReviewDecision}`. **An external process that is the JSON-RPC client
on that socket answers approvals by replying to these requests.** That is a real, supported
approval channel, not a workaround.

**3. State and metrics are also on that channel.** Relevant notifications
(`common.rs:1881-1916`): `thread/started`, `thread/status/changed`, `thread/closed`,
`turn/started`, `turn/completed`, `turn/diff/updated`, `turn/plan/updated`, `item/started`,
`item/completed`, `thread/tokenUsage/updated`, `item/autoApprovalReview/started` and `/completed`.
Relevant client requests: `thread/list`, `thread/loaded/list` ("Thread ids for sessions currently
loaded in memory", `v2/thread.rs:1633`), `thread/read`, `thread/turns/list`, `thread/items/list`,
`turn/start`, `turn/steer`, `turn/interrupt`, `account/rateLimits/read`, `account/usage/read`.

`thread/loaded/list` + `thread/status/changed` is a complete answer to "which agents exist and what
is each doing", with no file watching at all.

**Other channels, for completeness:**
- `codex mcp-server`: "Start Codex as an MCP server (stdio)". A separate, MCP-shaped surface.
- `codex remote-control`: "[experimental] Manage the app-server daemon with remote control
  enabled", with pairing (`remoteControl/pairing/start`, `remoteControl/client/list`).
- `codex agents`: a TUI that browses "all agent sessions on the shared local app-server daemon".
  Proof that cross-process session enumeration is an intended, first-class capability.
- **The sqlite db is NOT such a channel.** `~/.codex/sqlite/codex-dev.db` contains only
  `inbox_items`, `automations`, `automation_runs`, which is desktop-app automation scheduling. No approval
  queue, no session state.

---

## 8. Fixtures

| File | Provenance |
|---|---|
| `helper/test/fixtures/session-sample.jsonl` | **Real.** Captured 2026-09-07 with codex-cli 0.153.4. 9 lines, 67 KB. Copied verbatim from `~/.codex/sessions/2026/09/07/rollout-2026-09-07T06-20-00-01a07a18-2f55-78c3-9976-e71905ebb698.jsonl`. Line kinds in order: `session_meta`, `task_started`, `response_item`(message), `response_item`(message), `world_state`, `turn_context`, `response_item`(message), `item_completed`(UserMessage), `task_complete`(with error). |
| `helper/test/fixtures/session-sample-synthetic.jsonl` | **Synthetic, hand-assembled from source.** First line is a `_SYNTHETIC_NOTE` marker. Covers `token_usage_record`, `token_count` (with rate limits), `item_completed`(AgentMessage), a successful `task_complete` with `last_agent_message`, `turn_aborted`, and the two approval payloads (each preceded by a note that they are never written to the rollout). Field names and types are verified against the structs; **values are invented**. |

Why the real capture is thin: `codex exec` failed with
`Your access token could not be refreshed because your refresh token was already used. Please log
out and sign in again.` (HTTP 401 on `wss://chatgpt.com/backend-api/codex/responses`). No
re-authentication was attempted. The `--json` stdout stream for that run was:

```json
{"type":"thread.started","thread_id":"01a07a18-2f55-78c3-9976-e71905ebb698"}
{"type":"item.completed","item":{"id":"item_0","type":"error","message":"Model metadata for `gpt-5-codex` not found. Defaulting to fallback metadata; this can degrade performance and cause issues."}}
{"type":"turn.started"}
{"type":"error","message":"Your access token could not be refreshed because your refresh token was already used. Please log out and sign in again."}
{"type":"turn.failed","error":{"message":"Your access token could not be refreshed because your refresh token was already used. Please log out and sign in again."}}
```

Worth noting on its own: **`codex exec --json` on stdout is a fourth viable channel** if the helper
is the one spawning Codex. It is dot-cased (`thread.started`, `turn.started`, `item.completed`,
`turn.failed`) and does **not** share the rollout's snake_case tags. Do not write one parser for both.

---

## 9. UNCONFIRMED

Everything here is either untested on this machine or inferred. None of it should be treated as fact.

1. **`token_count` and `token_usage_record` have never been observed on disk here.** The turn died
   on auth before any usage was reported. Their shapes come from struct definitions plus
   `core/tests/suite/token_usage_rollout.rs`. Field names are high confidence; exact null-vs-omitted
   behavior for `rate_limits` sub-objects is UNCONFIRMED.
2. **Rate-limit values have never been observed.** No `RateLimitSnapshot` has been seen from a live
   account. Whether `primary` is the 5-hour and `secondary` the weekly window is an assumption from
   field ordering, not verified.
3. **Approval events have never been observed**, on disk or on the wire. The claim that they never
   reach the JSONL is confirmed from `rollout/src/policy.rs` (strong, it is an exhaustive match) but
   was not confirmed empirically, because no turn ever ran.
4. **The app-server control socket has never been opened.** `~/.codex/app-server-control/` does not
   exist on this machine. The daemon was not started. Socket path, JSON-RPC framing, handshake, and
   whether an unprivileged external process can attach are all UNCONFIRMED in practice.
5. **`ThreadStatus` / `ThreadActiveFlag` have never been observed on the wire.** Read from
   `app-server-protocol/src/protocol/v2/thread.rs:1645-1662`.
6. **The exact v2 approval request/response params were not read**: only the method names
   (`item/commandExecution/requestApproval` etc.) and the deprecated v1 param structs. The v2
   payload shapes are UNCONFIRMED.
7. **Behavior in `legacy` history mode is untested.** Only a `paginated` session was captured. The
   legacy/paginated split in section 4 is from source only.
8. **Multi-turn and compaction behavior is untested.** No `compacted` line, no second turn, no
   `turn_context` change mid-session was observed at 0.153.4. Whether `ordinal` is strictly
   contiguous across compaction or revert is UNCONFIRMED.
9. **Sub-agent / multi-agent rollouts are untested.** `agent_nickname`, `agent_role`, `agent_path`,
   `parent_thread_id`, `subagent_history_start_ordinal`, and the reverted-thread filename form
   `rollout-<ts>-<thread_id>_<rollout_id>.jsonl` were never seen on disk.
10. **`--ephemeral` was not tested.** The claim that it writes no rollout file comes from the flag's
    help text ("Run without persisting session files to disk").
11. **Tail-safety under concurrent readers is untested.** The flush-per-line claim is from
    `rollout/src/recorder.rs:1997-2001`; no concurrent-read race test was run.
12. **`model_context_window: 258400`** was observed once, for `gpt-5-codex`, in a run that also
    warned "Model metadata for `gpt-5-codex` not found. Defaulting to fallback metadata." That value
    may be a fallback default rather than the model's true window.
13. **Version drift risk.** The clone is HEAD-of-main from 2026-09-07, which is not necessarily the
    exact source of the published 0.153.4 npm build. Field-level drift between the two is possible.
14. **`brew upgrade codex` was never verified as having been offered a newer version.** The log
    contained only `codex exit 0`; whether Homebrew simply has a stale formula or the upgrade
    silently failed was not determined.

---

## 10. Source file index

| What | Path (under `vendor/codex/codex-rs/`) |
|---|---|
| `RolloutItem`, `RolloutLine` | `history/src/lib.rs:118`, `:254` |
| Wire tags for rollout lines | `history/src/rollout_payload.rs:22-60` |
| **Persistence policy (which events hit disk)** | `rollout/src/policy.rs` (whole file) |
| JSONL writer, flush-per-line | `rollout/src/recorder.rs:1977-2003` |
| Session dir layout, filename parsing | `rollout/src/list.rs:436`, `:1653`; `rollout/src/lib.rs:82` |
| Filename forms incl. reverted | `rollout/src/rollout_file_name_tests.rs:18-22` |
| `EventMsg` enum + serde tagging | `protocol/src/protocol.rs:1353-1556` |
| `task_started` / `task_complete` renames | `protocol/src/protocol.rs:1405`, `:1414` |
| `TurnCompleteEvent` | `protocol/src/protocol.rs:2141` |
| `TurnStartedEvent` | `protocol/src/protocol.rs:2167` |
| `TokenUsage` | `protocol/src/protocol.rs:2216` |
| `TokenUsageRecord` | `protocol/src/protocol.rs:2239` |
| `TokenUsageInfo` | `protocol/src/protocol.rs:2251` |
| `TokenCountEvent` | `protocol/src/protocol.rs:2318` |
| `RateLimitSnapshot` | `protocol/src/protocol.rs:2324` |
| `RateLimitWindow` | `protocol/src/protocol.rs:2367` |
| `BASELINE_TOKENS = 12000` | `protocol/src/protocol.rs:2394` |
| `percent_of_context_window_remaining` | `protocol/src/protocol.rs:2428` |
| `SessionContextWindow` (window_id, not size) | `protocol/src/protocol.rs:3007` |
| `SessionMeta` | `protocol/src/protocol.rs:3040` |
| `SessionMetaLine` + session_id backfill | `protocol/src/protocol.rs:3137-3170` |
| `TurnContextItem` | `protocol/src/protocol.rs:3202` |
| `TurnAbortedEvent`, `TurnAbortReason` | `protocol/src/protocol.rs:4154`, `:4172` |
| `ExecApprovalRequestEvent` | `protocol/src/approvals.rs:262` |
| `ApplyPatchApprovalRequestEvent` | `protocol/src/approvals.rs:449` |
| `TurnItem` variants | `protocol/src/items.rs:45-77` |
| Control socket path + constants | `app-server-transport/src/transport/mod.rs:55-65` |
| RPC method table (client + server requests) | `app-server-protocol/src/protocol/common.rs:507-1449`, `:1728-1794` |
| Notification table | `app-server-protocol/src/protocol/common.rs:1881-1916` |
| `ThreadStatus`, `ThreadActiveFlag` | `app-server-protocol/src/protocol/v2/thread.rs:1645-1662` |
| `ThreadLoadedListResponse` | `app-server-protocol/src/protocol/v2/thread.rs:1633` |
| `ThreadStatusChangedNotification` | `app-server-protocol/src/protocol/v2/thread.rs:1946` |
| `ThreadTokenUsageUpdatedNotification` | `app-server-protocol/src/protocol/v2/thread.rs:1845` |
| v1 approval params/responses | `app-server-protocol/src/protocol/v1.rs:139-178` |
| Token accounting semantics test | `core/tests/suite/token_usage_rollout.rs:89-105` |
