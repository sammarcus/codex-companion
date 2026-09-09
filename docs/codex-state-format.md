# Codex CLI on-disk state format

How a host helper reads metrics out of `~/.codex/`, and why it cannot read
*state* out of there.

**What shipped, and what this document is for.** State comes from Codex's
first-party hook events, not from disk. The rollout file is read for **numbers
only**, which is what sections 3 to 6 are the reference for. The app-server
control socket described here was fully mapped and then not used; the reasoning
and the traps are in `docs/prior-art.md`, and `docs/architecture.md` section 6
is the shipped design.

**Version basis:** `codex-cli 0.153.4` (npm `@openai/codex@latest`, installed to an isolated
prefix, see "Install situation"). Source read from `openai/codex` at commit
`121f91fd5d9dc66017866ce9bdc49f1e182721df` (2026-09-07), cloned to
this repo's `vendor/codex`.

**Empirical basis:** one real session captured on 2026-09-07 with 0.153.4, at
`~/.codex/sessions/2026/09/07/rollout-2026-09-07T06-20-00-01a07a18-2f55-78c3-9976-e71905ebb698.jsonl`,
copied to `helper/test/fixtures/session-sample.jsonl` (with two redactions, see section 8).
Its turn **failed on expired auth**, so it
contains the session/turn lifecycle lines but no model output, no token accounting, and no
approval traffic. Those event shapes are documented from source and hand-assembled into
`helper/test/fixtures/session-sample-synthetic.jsonl`, which is explicitly marked synthetic.

---

## 0. Headline findings

1. **Approval requests are NOT in the rollout JSONL.** `rollout/src/policy.rs`
   classifies `EventMsg::ExecApprovalRequest` and
   `EventMsg::ApplyPatchApprovalRequest` as transient, non-durable events and
   returns `false` for them. A helper that only tails `~/.codex/sessions/`
   **cannot** see "waiting for approval". This is the single most important
   constraint on the design, and it is why the state path is the hook system.
2. **There is a programmatic approval channel**, the app-server's Unix control
   socket, and it also exposes an explicit state machine (`ThreadStatus` =
   `notLoaded | idle | systemError | active{activeFlags:[waitingOnApproval |
   waitingOnUserInput]}`) pushed as `thread/status/changed`. Neither is
   derivable from disk. Confirmed, mapped, and deliberately not used: see
   `docs/prior-art.md`.
3. Rollout writes are **line-atomic for tailing**: every record is one
   `write_all(json + "\n")` followed immediately by `flush()`
   (`rollout/src/recorder.rs:1997-2001`). No partial lines under normal
   operation.
4. Recent rollouts use `history_mode: "paginated"`, which changes which events
   are persisted. The real capture confirms it.

**Version basis:** `codex-cli 0.153.4`, source read from `openai/codex` at
commit `121f91fd5d9dc66017866ce9bdc49f1e182721df` (2026-09-07), cloned to
`vendor/codex`.

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

## 6. Reading the numbers

**State is not derived from here.** An earlier design tailed the rollout and
inferred `BUSY` from an unmatched `task_started`, with a stall timeout standing
in for "waiting". It cannot work: long model turns and long tool calls emit
**nothing** to the JSONL, because `exec_command_begin/end`, `item_started` and
all delta events are non-durable, so silence is not evidence of being stuck.
And per finding 1 above, approvals never reach disk at all. Codex's
`PermissionRequest` hook event replaced the whole heuristic.

What the rollout is still good for is numbers.

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
  `thread_token_usage`. `applyUsage` in `helper/codex-companion.js` keeps the two in separate
  fields for exactly this reason: `contextTokens` (fed from `last_token_usage` / `usage`) is what
  reaches `contextFill()` and the ring, `totalTokens` (fed from the cumulative figures) is
  display-only accounting and never touches the ring.

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
  `token_usage_record` (section 5), not here. The turn-delta logic lives in `applyEvent` in
  `helper/codex-companion.js`.
  Label it derived. It is an average over the interval, not an instantaneous rate, and it excludes
  time spent in tool calls.

- **Cached vs fresh input.** `input_tokens` **includes** `cached_input_tokens`. Codex computes
  `non_cached_input = max(input_tokens - cached_input_tokens, 0)` and
  `blended_total = non_cached_input + max(output_tokens, 0)` (`protocol.rs:2415-2426`). Use
  `blended_total` if you want a single "work done" number.

- **Rate limits.** Read the most recent `token_count.payload.rate_limits`. It only updates when a
  `token_count` event fires, so it can be stale between turns.

### The app-server, in one paragraph

Subscribing to `thread/status/changed` and reading `ThreadStatus` directly is
strictly better than any disk heuristic, and it is the only correct way to get
"waiting" from a channel other than the hooks. It was confirmed to exist on
0.153.4: `codex app-server --help` lists a `proxy` subcommand, and the socket
path is built by `app_server_control_socket_path`
(`app-server-transport/src/transport/mod.rs:59-65`) as
`$CODEX_HOME/app-server-control/app-server-control.sock`. It was not used, and
`docs/prior-art.md` carries the approval response shapes, the framing, the
WebSocket-over-Unix-socket trap and the fork hazard on `thread/resume`, so none
of that has to be rediscovered if it is ever wanted.

The sqlite databases under `~/.codex/` are **not** such a channel:
`codex-dev.db` holds only `inbox_items`, `automations` and `automation_runs`,
which is desktop-app scheduling. No approval queue, no session state.

---

## 8. Fixtures

| File | Provenance |
|---|---|
| `helper/test/fixtures/session-sample.jsonl` | **Real, with two redactions.** Captured 2026-09-07 with codex-cli 0.153.4. 9 lines, 32 KB. Redacted before publishing: the home directory of the recording machine is written `/Users/you`, and the injected skills listing (lines 3 and 5) keeps one entry as a shape example in place of that machine's installed skills. Every other byte is as captured, and no line, kind, field or number changed. Line kinds in order: `session_meta`, `task_started`, `response_item`(message) x2, `world_state`, `turn_context`, `response_item`(message), `item_completed`(UserMessage), `task_complete`(with error) |
| `helper/test/fixtures/session-sample-synthetic.jsonl` | **Synthetic, hand-assembled from source.** First line is a `_SYNTHETIC_NOTE` marker. Covers `token_usage_record`, `token_count` (with rate limits), `item_completed`(AgentMessage), a successful `task_complete`, `turn_aborted`, and the two approval payloads (each preceded by a note that they are never written to the rollout). Field names and types are verified against the structs; **values are invented** |

The real capture is thin because the turn failed on expired auth (HTTP 401 on
the responses endpoint) before any model output or token accounting. That is
also the whole of open question L1.

**One thing worth knowing from that run: `codex exec --json` on stdout is a
fourth viable channel** if the helper is the one spawning Codex. It is dot-cased
(`thread.started`, `turn.started`, `item.completed`, `turn.failed`) and does
**not** share the rollout's snake_case tags. Do not write one parser for both.

---

## 9. What is still unconfirmed

Every open item from this document, with the command that settles it, is in
`docs/open-questions.md` (rows L1, L2, L7 to L14). The two that matter: the
`token_count` and `token_usage_record` line shapes have **never been observed on
disk here**, because the one real capture died on expired auth before any usage
was reported, and their shapes come from struct definitions plus
`core/tests/suite/token_usage_rollout.rs`. And `model_context_window: 258400`
was seen once, in a run that also warned about missing model metadata, so it may
be a fallback default rather than the model's real window.

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
