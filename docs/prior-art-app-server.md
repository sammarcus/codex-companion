# Prior art: Codex app-server JSON-RPC clients

Survey of six independent client libraries plus a wider GitHub sweep, cross-checked against the
authoritative protocol schema dumped by `codex app-server generate-json-schema` from codex-cli
0.153.4 on this machine.

Schema copy used for every cross-check:
`/private/tmp/claude-501/-Users-sam-claude-codex-buddy/01d4d4d3-57c4-4db0-9b6b-9d7df0da23b4/scratchpad/schema/`
(38 top-level files, `v1/` with 2 files, `v2/` with 265 files, plus two combined bundles).

Upstream source cross-checks were read through `gh api repos/openai/codex/contents/...`, never cloned.

Ordering below leads with items 5 and 6 because those are the load-bearing ones for a device that
must observe and answer for a human's own terminal session.

---

## 5. How to answer an approval (the exact wire shape)

### 5.1 The envelope

The server sends a JSON-RPC **request** (it has an `id`), you reply with a JSON-RPC **result** on
the same `id`. Schema: `schema/JSONRPCRequest.json` (`required: ["id", "method"]`) and
`schema/JSONRPCResponse.json` (`required: ["id", "result"]`).

**There is no `jsonrpc` field.** Neither schema declares one, and the upstream serializer test
proves the server omits it on the wire:

`codex-rs/app-server-transport/src/transport/mod.rs:353-357`
```rust
let json = serialize_outgoing_message(message).expect("message should serialize");
assert_eq!(
    serde_json::from_str::<serde_json::Value>(&json).expect("message should be valid JSON"),
    json!({ "id": 7, "result": {} })
);
```

Server notifications likewise carry `method` + `params` plus an extra `emittedAtMs`, and no
`jsonrpc` (`transport/mod.rs:330-341`).

Practical note: `messon007/codex-thread-studio` omits `jsonrpc` entirely
(`src-tauri/src/codex_app_server.rs:224-244`) and works. `luisjpf` and `GitHoobar` both send
`"jsonrpc": "2.0"` and also work, so the field is tolerated but not required. Sending it is
harmless. Requiring it on inbound parsing would be a bug.

### 5.2 The three modern approval requests and their exact responses

| Server request method | Response schema | Response body |
|---|---|---|
| `item/commandExecution/requestApproval` | `schema/CommandExecutionRequestApprovalResponse.json` | `{"decision": <CommandExecutionApprovalDecision>}` |
| `item/fileChange/requestApproval` | `schema/FileChangeRequestApprovalResponse.json` | `{"decision": <FileChangeApprovalDecision>}` |
| `item/permissions/requestApproval` | `schema/PermissionsRequestApprovalResponse.json` | `{"permissions": {...}, "scope": "turn"\|"session", "strictAutoReview": bool\|null}` |

Method names are confirmed from `schema/ServerRequest.json` (extracted enums): the complete
server-to-client request set is `item/commandExecution/requestApproval`,
`item/fileChange/requestApproval`, `item/permissions/requestApproval`, `item/tool/requestUserInput`,
`item/tool/call`, `mcpServer/elicitation/request`, `account/chatgptAuthTokens/refresh`,
`attestation/generate`, and the two legacy ones `applyPatchApproval` and `execCommandApproval`.

#### CommandExecutionApprovalDecision (the one that matters most)

`schema/CommandExecutionRequestApprovalResponse.json`, `definitions.CommandExecutionApprovalDecision`
is a `oneOf` of six variants. Four are bare strings, two are objects:

```jsonc
// approve this one command
{"id": <same id>, "result": {"decision": "accept"}}

// approve and stop asking for equivalents this session
{"id": <same id>, "result": {"decision": "acceptForSession"}}

// deny, agent keeps working and may try another approach
{"id": <same id>, "result": {"decision": "decline"}}

// deny and interrupt the turn immediately
{"id": <same id>, "result": {"decision": "cancel"}}

// approve and persist an execpolicy amendment
{"id": <same id>, "result": {"decision": {"acceptWithExecpolicyAmendment": {
  "execpolicy_amendment": ["<rule>", "..."]
}}}}

// approve and persist a per-host network rule
{"id": <same id>, "result": {"decision": {"applyNetworkPolicyAmendment": {
  "network_policy_amendment": {"host": "example.com", "action": "allow"}
}}}}
```

Note the snake_case keys inside the two object variants. Everything else in the protocol is
camelCase; these two are not. That is straight from the schema, not a typo.

#### FileChangeApprovalDecision

`schema/FileChangeRequestApprovalResponse.json` allows exactly four bare strings, no object
variants: `accept`, `acceptForSession`, `decline`, `cancel`.

```jsonc
{"id": <same id>, "result": {"decision": "accept"}}
```

#### Permissions approval is a different shape entirely

`item/permissions/requestApproval` does **not** take a `decision`. Its response
(`schema/PermissionsRequestApprovalResponse.json`) requires a `permissions` object of type
`GrantedPermissionProfile`:

```jsonc
{"id": <same id>, "result": {
  "permissions": {
    "fileSystem": {
      "entries": [{"path": {"type": "path", "path": "/abs/path"}, "access": "write"}],
      "globScanMaxDepth": null
    },
    "network": {"enabled": true}
  },
  "scope": "turn",              // or "session"; defaults to "turn"
  "strictAutoReview": null
}}
```

To **deny** a permissions request, grant nothing: `{"permissions": {}}` (both `fileSystem` and
`network` are nullable and the object has no required fields). There is no "deny" token. Path
entries can be `{"type":"path","path":...}`, `{"type":"glob_pattern","pattern":...}`, or
`{"type":"special","value":{"kind":"root"|"minimal"|"project_roots"|"tmpdir"|"slash_tmp"|"unknown"}}`.
`access` is one of `read`, `write`, `deny`.

#### Legacy `execCommandApproval` / `applyPatchApproval`

Both take `{"decision": <ReviewDecision>}` and `ReviewDecision` is a **different vocabulary**:
`approved`, `approved_for_session`, `approved_mcp_policy_amendment`, `timed_out`, `abort` as bare
strings, plus objects `{"approved_execpolicy_amendment": {"proposed_execpolicy_amendment": [...]}}`,
`{"network_policy_amendment": {"network_policy_amendment": {...}}}`, and critically
`{"denied": {"rejection": "<string>"}}`.

Deny in the legacy vocabulary is an **object**, not the bare string `"denied"`. See
`schema/ExecCommandApprovalResponse.json` and `schema/ApplyPatchApprovalResponse.json`,
`definitions.ReviewDecision`, variant title `DeniedReviewDecision`.

#### `item/tool/requestUserInput` (this is the `waitingOnUserInput` one)

Params (`schema/ToolRequestUserInputParams.json`) require `threadId`, `turnId`, `itemId`,
`questions`, `isBlocking`. The response (`schema/ToolRequestUserInputResponse.json`) is a map from
question id to answers:

```jsonc
{"id": <same id>, "result": {"answers": {"<questionId>": {"answers": ["text"]}}}}
```

Marked EXPERIMENTAL in the schema description.

### 5.3 Never leave a server request unanswered

`financialvice/codex-app-server-client` documents the failure mode explicitly:

`Sources/CodexAppServerClient/CodexAppServerClient.docc/HandlingApprovals.md:87-94`
> "Reject them explicitly rather than leaving them unanswered, an unanswered server request stalls
> the turn indefinitely" ... "JSON-RPC code `-32601` ('method not found') is the conventional signal
> that the client does not support the method."

So for anything you do not model, reply:
```jsonc
{"id": <same id>, "error": {"code": -32601, "message": "not implemented"}}
```
Error object shape from `schema/JSONRPCErrorError.json` (`required: ["code", "message"]`, optional
`data`).

`JaminZhou` does this automatically: README.md:280, "Unhandled server requests receive JSON-RPC
method-not-found. The client does not auto-approve commands or file changes."

`unstableneutron/pi-toolbox` does the same by hand, answering only MCP elicitation and erroring on
everything else (`extensions/pi-codex-app-server-use/scripts/codex-control.mjs:326-327`).

### 5.4 Client-by-client comparison, and one that is wrong

**JaminZhou (TypeScript), correct and closest to what a Node helper wants.**
`README.md:99-101`:
```ts
client.onServerRequest("item/commandExecution/requestApproval", () => ({
  decision: "decline",
}));
```
`README.md:270-273`:
```ts
client.onServerRequest("item/fileChange/requestApproval", async (params) => {
  console.log(params.itemId, params.reason);
  return { decision: "decline" };
});
```
Dispatch plumbing at `src/app-server-client.ts:835-847`. Matches the schema exactly.

**financialvice (Swift), correct, and it publishes the cross-vocabulary mapping table.**
`Sources/CodexAppServerProtocol/Support/ApprovalDecision.swift:8-13`:

| Intent | ReviewDecisionEnum | FileChangeApprovalDecision |
|---|---|---|
| allowOnce | `.approved` | `.accept` |
| allowForSession | `.approvedForSession` | `.acceptForSession` |
| deny | `.denied` | `.decline` |
| abort | `.abort` | `.cancel` |

Caveat to check if you copy this table: the `.denied` cell is the legacy `ReviewDecision`, and per
`schema/ExecCommandApprovalResponse.json` that variant serializes as the object
`{"denied": {"rejection": "..."}}`, not as the bare string `"denied"`. The Swift source is a doc
comment, so the actual `Codable` encoding may carry the associated value correctly; the table alone
would mislead a reimplementation. Also note the Swift client covers only four approval cases
(`HandlingApprovals.md:43-48`) and deliberately rejects `itemPermissionsRequestApproval` with
`-32601` (`HandlingApprovals.md:87-92`).

**paras-anekantvad (Python), incomplete and adds a field that does not exist.**
`src/codex_app_server_client/types/approvals.py:32-44`:
```python
class ApprovalDecision(StrEnum):
    ACCEPT = "accept"
    DECLINE = "decline"
    CANCEL = "cancel"

class CommandExecutionApprovalResponse(CamelModel):
    decision: ApprovalDecision
    accept_settings: dict[str, Any] | None = None
```
Two disagreements with the schema: `acceptForSession` is missing (so is
`acceptWithExecpolicyAmendment` and `applyNetworkPolicyAmendment`), and `acceptSettings` is not a
property of `CommandExecutionRequestApprovalResponse` in the schema at all. Its
`FileChangeApprovalRequest` also omits `startedAtMs`, which the schema marks required
(`schema/FileChangeRequestApprovalParams.json`, `required: ["itemId","startedAtMs","threadId","turnId"]`).

**luisjpf (Rust CLI), sends a payload that does not match any response schema. Do not copy.**
`src/approval.rs:261-269`:
```rust
pub fn approval_response_payload(approval: &Approval, decision: &ApprovalDecision) -> Value {
    json!({
        "approved": decision.approved,
        "decision": if decision.approved { "approved" } else { "denied" },
        "resume": decision.resume,
        "resumeToken": approval.resume_token,
        "approvalId": approval.approval_id,
    })
}
```
sent as the JSON-RPC `result` at `src/client/connection.rs:460-471`. It routes
`item/commandExecution/requestApproval` (matched at `src/approval.rs:34-36`) but replies with
`"decision": "approved"` / `"denied"`, which are legacy `ReviewDecision` tokens, and `"denied"` is
not even a valid bare string there. The extra `approved`, `resume`, `resumeToken`, `approvalId`
keys are its own CLI abstraction leaking onto the wire. Its `approvals.md` documents the CLI
envelope (`approval_id`, `resume_token`, exit code 7), which is a local persistence format, not the
protocol. Treat this repo as a UX reference only.

**GitHoobar (Rust)** has no approval handling at all (zero hits for `requestApproval`, `decision`,
or `Approval` in `src/client.rs`).

**messon007 (thread-studio)** proxies raw messages between its UI and the app-server rather than
modelling approvals in Rust, so its approval shape lives in `ui/app.js` rather than the transport
layer.

### 5.5 The multi-subscriber wrinkle you need to know about

`openai/codex` emits a `serverRequest/resolved` notification
(`schema/v2/ServerRequestResolvedNotification.json`, `{requestId, threadId}`). That exists precisely
because more than one connected client can see the same approval request. openclaw's own spec puts
it plainly (`docs/specs/codex-supervision.md`, quoted by the subagent survey):

> "Approval requests can also reach every subscriber of one server, with the first valid response
> completing the request."

So: your device answering an approval is legitimate and will complete the request, and the human's
TUI will get `serverRequest/resolved` for that `requestId`. Conversely, if the human clicks in the
TUI first, you get `serverRequest/resolved` and must drop your pending prompt. Handle both races.

---

## 6. Attaching to a session the user started in their own terminal

**Yes, this is possible, and it is the intended architecture. It hinges on one thing: everybody has
to be talking to the *same* app-server process.**

### 6.1 The wrong mental model

Spawning `codex app-server` yourself gives you a brand new process with its own empty in-memory
thread table. `thread/loaded/list` on that process returns `[]` forever, no matter what the user is
doing in their terminal. Four of the six surveyed clients do exactly this and therefore cannot
observe a foreign session:

- `JaminZhou`: `src/app-server-client.ts:787` spawns
  `codex app-server --listen stdio://`, `src/app-server-client.ts:789`.
- `paras-anekantvad`: `src/codex_app_server_client/protocol/transport.py:38` and `:128`, both
  `[codex_bin, *extra_args, "app-server", "--listen", "stdio://"]`.
- `GitHoobar`: `src/transport.rs:98-101`, `Command::new(cmd_path).arg("app-server")`.
- `messon007`: `src-tauri/src/codex_app_server.rs:106`, `arguments.extend(["app-server", "--stdio"])`.

`financialvice` also spawns (`Sources/CodexAppServerClient/CodexProcessLauncher.swift`).
`luisjpf` is the odd one out: it is WebSocket-only and connects to an already-listening server
(`src/client/connection.rs:648`, `connect_async(request)`), with stdio explicitly marked
`status: "planned"` (`src/client/stdio.rs:12-13`).

### 6.2 The right mental model: the shared control socket

`codex` exposes a per-user control socket:

`codex-rs/app-server-transport/src/transport/mod.rs:55-65`
```rust
const APP_SERVER_CONTROL_SOCKET_DIR_NAME: &str = "app-server-control";
const APP_SERVER_CONTROL_SOCKET_FILE_NAME: &str = "app-server-control.sock";

pub fn app_server_control_socket_path(codex_home: &Path) -> std::io::Result<AbsolutePathBuf> {
    AbsolutePathBuf::from_absolute_path(
        codex_home
            .join(APP_SERVER_CONTROL_SOCKET_DIR_NAME)
            .join(APP_SERVER_CONTROL_SOCKET_FILE_NAME),
    )
}
```
so the path is `$CODEX_HOME/app-server-control/app-server-control.sock`, defaulting to
`~/.codex/app-server-control/app-server-control.sock`.

`--listen` accepts, per `mod.rs:93-96` and `mod.rs:117-156`:
`stdio://` (default), `unix://` (resolves to the control socket path above), `unix://PATH`,
`ws://IP:PORT`, `off`.

Every connection to the same server is a peer:
`mod.rs:176-197` gives each accepted connection a `ConnectionId` and a `ConnectionOrigin` of
`Stdio | InProcess | WebSocket | RemoteControl`. The server multiplexes them.

### 6.3 The discovery call: `thread/loaded/list`

Present in the authoritative `ClientRequest` method list (extracted from `schema/ClientRequest.json`).

`schema/v2/ThreadLoadedListParams.json` takes optional `cursor` and `limit`.
`schema/v2/ThreadLoadedListResponse.json`:
```jsonc
{"data": ["<threadId>", ...], "nextCursor": "..." | null}
```
with `data` described as "Thread ids for sessions currently loaded in memory." That is exactly the
set of live sessions on that server, which is the set of terminal sessions the user has open, when
you are attached to the shared server.

Only one of the six surveyed clients calls it:
`paras-anekantvad/codex-app-server-client-sdk/src/codex_app_server_client/_sync_client.py:270-272`
```python
def thread_loaded_list(self) -> ThreadLoadedListResponse:
    result = self.request("thread/loaded/list", {}, timeout_s=30)
    return ThreadLoadedListResponse.model_validate(result)
```
which is useless in that SDK because it spawns its own server (see 6.1). It is nonetheless a
correct signature.

There is **no `thread/subscribe` method.** The full `ClientRequest` method list contains
`thread/unsubscribe` but no subscribe counterpart. Subscription is a side effect of
`thread/resume` (and of `thread/start` for threads you create). `thread/unsubscribe` takes
`{"threadId": "..."}` (`schema/v2/ThreadUnsubscribeParams.json`).

### 6.4 Read-only observation without side effects

Three patterns found in the wild, in increasing invasiveness:

**(a) Catalog only, zero subscription.** `openclaw/openclaw`
`extensions/codex/src/supervision-tools.ts:583-621` (`listLoadedSessions`) calls
`thread/loaded/list` at line 591, then `thread/read` per id at `supervision-tools.ts:558-577`. Its
spec is explicit: "Catalog discovery is passive. Listing or reading metadata must not call
`thread/resume`, subscribe the OpenClaw client to live thread requests, or answer an approval."
`thread/resume` is only reached when a human opens a supervised chat
(`extensions/codex/src/app-server/thread-resume.ts:50`).

`thread/read` params are `{threadId, includeTurns: bool}` and the schema warns that
"Full-history hydration is deprecated for paginated threads" (`schema/v2/ThreadReadParams.json`), so
prefer `includeTurns: false` plus `thread/items/list` or `thread/turns/list`.

**(b) Loaded-check then act, never resume.** `pchalasani/claude-code-tools`
`claude_code_tools/codex_app_server_rpc.py:183-199` (`loaded_thread_ids`) calls
`thread/loaded/list`, then `verify_thread_loaded` / `deliver_thread_message`
(`codex_app_server_rpc.py:221, 248`) use `thread/read` and reject `notLoaded` / `systemError`, then
push `turn/start` or `turn/steer` into the already-live thread. `thread/resume` is never called.
Its TS twin refuses to answer server requests at all:
`plugins/dynamic-workflow/src/app-server-client.ts:279-283`
```ts
// Server-initiated requests are deliberately left unanswered. The TUI is another
// subscriber and owns approvals and user-input requests.
if (message.id !== undefined) { return; }
```
That is the opposite of what your device wants, but it is direct evidence that a non-originating
connection **does** receive the approval server requests for a foreign thread. It has to explicitly
choose not to answer them.

**(c) Guarded resume.** `lucianlamp/codex-monitor` `src/client.rs:71-88` (`ensure_thread_loaded`)
refuses `thread/resume` unless the id is already in `thread/loaded/list`, with the error string
"thread {thread_id} is not loaded in the target app-server; refusing to call thread/resume because
it can fork." That is the safety rule to copy: **`thread/resume` on a thread that is not loaded can
fork it.** On a thread that *is* loaded, resume is the subscribe primitive.

`codex-monitor` also shows how to find the endpoint when you do not know it: it shells out to
`ps -axo command=` (`src/target.rs:181-183`) and parses the `--listen` value out of a live
`codex app-server` argv (`src/target.rs:344-355`), with recognized fixtures
`codex app-server --listen unix:///tmp/server.sock` (`target.rs:524`) and
`codex app-server --listen ws://127.0.0.1:54015` (`target.rs:527`), falling back to
`~/.codex/app-server-control/app-server-control.sock` (`target.rs:427`).

### 6.5 Notification suppression, useful for a low-power device

`initialize` capabilities include `optOutNotificationMethods`, "Exact notification method names that
should be suppressed for this connection (for example `thread/started`)"
(`schema/v1/InitializeParams.json`, `definitions.InitializeCapabilities`).

`pchalasani` uses `optOutNotificationMethods: ["*"]` for the pure-catalog connection
(`codex_app_server_rpc.py:186-196`), and for the working connection opts out of only the four
high-frequency streams: `item/agentMessage/delta`, `item/commandExecution/outputDelta`,
`item/reasoning/summaryTextDelta`, `thread/tokenUsage/updated`. For a device that only cares about
"is it waiting on me", opting out of every `*/delta` method plus `item/started` / `item/completed`
would cut the traffic to almost nothing.

### 6.6 Prerequisite: the daemon must be running

On this machine, `~/.codex/app-server-control/` does not exist and
`~/.codex/app-server-daemon/` contains only zero-byte `app-server.pid.lock` and `daemon.lock`, so no
daemon has ever successfully started here (confirmed by `ls`, 2026-09-07).

`codex app-server daemon --help` on codex-cli 0.153.4 lists:
`bootstrap` ("Install durable local app-server management for SSH-driven use"), `start`, `restart`,
`enable-remote-control`, `disable-remote-control`, `stop`, `version`. There is no `status`
subcommand.

Upstream, `codex-rs/app-server-daemon/src/lib.rs:23` imports
`app_server_control_socket_path`, `:35-39` names `app-server.pid`, `app-server-updater.pid`,
`daemon.lock`, `settings.json`, and state dir `app-server-daemon`, and `:280-286` resolves the
socket path from `CODEX_HOME`. `probe_app_server_version` (`lib.rs:82-85`) is the liveness check.

`unstableneutron/pi-toolbox` treats this as a hard prerequisite it does not satisfy itself
(`README.md:25-33`): "All capabilities use the global Codex AppServer daemon. The extension does not
spawn or maintain its own app-server process ... When any active capability needs the daemon and the
control socket is unavailable, the extension suppresses its tools for that session and warns."

**Open question for the device design:** does a plain `codex` TUI session in a terminal register its
thread with the shared daemon, or only with its own in-process server? Every observer in this survey
attaches to the daemon and assumes yes, and `ConnectionOrigin::InProcess`
(`transport/mod.rs:194`) suggests the TUI runs an in-process app-server that can be the same one the
control socket fronts. This was not provable here because the daemon will not start on this
install. Verify empirically on a machine where `codex app-server daemon start` works: start a TUI
session, then run `thread/loaded/list` over the control socket and see whether its thread id shows
up.

---

## 3. Connection strategy, and which is most robust

Three transports exist. What matters is that **they are not framed the same way.**

### stdio: newline-delimited JSON, no framing headers

`codex-rs/app-server-transport/src/transport/stdio.rs:45-46, 88-89`
```rust
let reader = BufReader::new(stdin);
let mut lines = reader.lines();
...
json.push('\n');
if let Err(err) = stdout.write_all(json.as_bytes()).await {
```

### unix control socket: WebSocket over AF_UNIX, not raw JSONL

This is the trap. The control socket does a full RFC 6455 handshake before any JSON flows.

Server side, `codex-rs/app-server-transport/src/transport/unix_socket.rs:107-134`:
```rust
let websocket_stream = match accept_hdr_async(
    ...
    |request: &tokio_tungstenite::tungstenite::handshake::server::Request, response| {
...
let (websocket_writer, websocket_reader) = websocket_stream.split();
run_websocket_connection(websocket_writer, websocket_reader, transport_event_tx).await;
```

Client side, `codex-rs/app-server-daemon/src/client.rs:63-71`:
```rust
pub(crate) async fn connect(socket_path: &Path) -> Result<WebSocketStream<UnixStream>> {
    connect_at(socket_path, "ws://localhost/").await
}

async fn connect_at(socket_path: &Path, url: &str) -> Result<WebSocketStream<UnixStream>> {
    let stream = UnixStream::connect(socket_path).await...;
    let (websocket, _response) = client_async(url, stream).await...
```

Every third-party client that touches the control socket confirms this: `openclaw`
(`extensions/codex/src/app-server/transport-websocket.ts:44-49, 212-214, 230-238`, joining
`"app-server-control"` + `"app-server-control.sock"` then wrapping `net.createConnection` as a
WebSocket), `codex-monitor` (`src/transport/unix.rs:26-30`), `pchalasani`
(`plugins/dynamic-workflow/src/app-server-client.ts:368-383`, literal `"app-server-control.sock"` at
line 380), `pi-toolbox` (`extensions/pi-codex-app-server-use/src/app-server-control.ts:5-8, 27`).

Note the daemon-shutdown side channel: connecting to `ws://localhost/daemon/shutdown` and sending
the pid as a text frame kills the daemon (`daemon-client.rs:80-89`, server side
`unix_socket.rs:141-152`). Do not send stray text frames on a fresh connection.

### ws://IP:PORT: TCP WebSocket, optionally authenticated

`mod.rs:151-156`. Auth is bolted on here, not on the unix socket:
`codex-rs/app-server-transport/src/transport/auth.rs:33-53` defines `--ws-token-file`,
`--ws-token-sha256`, `--ws-shared-secret-file`, `--ws-issuer`, `--ws-audience`,
`--ws-max-clock-skew-seconds`, with JWT validation via the `jsonwebtoken` crate. `luisjpf` sends
`Authorization: Bearer <token>` (`src/client/connection.rs:640-645`).

The unix socket is protected by directory permissions instead
(`app-server-daemon/src/lib.rs:770`, `codex_uds::prepare_private_socket_directory(parent)`).

### `codex app-server proxy`: the pragmatic answer for a zero-dependency Node client

From `codex app-server proxy --help` on 0.153.4:
> "Proxy stdio bytes to the running app-server control socket"
> `--sock <SOCKET_PATH>  Path to the app-server Unix domain socket to connect to`

That is the whole point of the subcommand: it performs the WebSocket handshake and unwraps frames
for you, and hands your process plain newline-delimited JSON on stdin/stdout. `openclaw` recognizes
this invocation shape as "forwarding to an external server it does not own"
(`extensions/codex/src/app-server/launch-args.ts:78-85`, `isCodexAppServerProxyLaunch`).

**Recommendation for the Node helper.** `child_process.spawn("codex", ["app-server", "proxy"])`,
then readline over stdout and `write(JSON.stringify(msg) + "\n")` to stdin. It gets you:
- the shared server (so `thread/loaded/list` sees the user's terminal threads),
- stdio JSONL framing (so no WebSocket implementation, no `ws` dependency, no frame masking),
- no long-lived listener you have to manage, and it dies with your process.

The cost is one extra child process and a dependency on the `codex` binary being on PATH, which you
already have.

If you insist on connecting to the socket directly from Node with zero dependencies, you must
implement the client half of RFC 6455: `Sec-WebSocket-Key` handshake over `net.createConnection`,
then masked text frames. That is roughly 150 lines and is the only reason `openclaw`,
`codex-monitor`, `pchalasani` and `pi-toolbox` all carry a WebSocket layer. Spawning `proxy` is
strictly less code.

Avoid `--listen ws://IP:PORT` unless you specifically want network reach, because it opens a TCP
port and then you need the token plumbing from `auth.rs`.

---

## 1. Handshake: the exact initialize sequence

Two messages from the client, one response from the server. It is not the MCP handshake.

1. Client request `initialize` with params `{clientInfo, capabilities?}`.
2. Server responds with `{userAgent, codexHome, platformOs, platformFamily}`.
3. Client sends the notification `initialized` with no params (or `{}`).

`schema/v1/InitializeParams.json`:
```jsonc
{
  "clientInfo": {"name": "<required>", "version": "<required>", "title": "<optional>"},
  "capabilities": {                      // whole object optional
    "experimentalApi": false,            // default false
    "requestAttestation": false,         // default false
    "extensions": {},                    // MCP extension settings, e.g. {"openai/form": {}}
    "mcpServerOpenaiFormElicitation": false,   // legacy opt-in, superseded by extensions
    "optOutNotificationMethods": ["thread/started"]
  }
}
```
Only `clientInfo` is required. Only `name` and `version` inside it are required.

`schema/v1/InitializeResponse.json`, all four required:
```jsonc
{
  "userAgent": "...",
  "codexHome": "/Users/you/.codex",     // AbsolutePathBuf
  "platformOs": "macos",                // "macos" | "linux" | "windows"
  "platformFamily": "unix"              // "unix" | "windows"
}
```

`schema/ClientNotification.json` has exactly one variant, `initialized`, with only `method` required
and no params.

### Real excerpt, `messon007/codex-thread-studio` (JS/Rust, closest to a hand-rolled Node client)

`src-tauri/src/codex_app_server.rs:222-245`, note the absent `jsonrpc`:
```rust
input.send(
    json!({
        "method": "initialize",
        "id": INITIALIZE_REQUEST_ID,
        "params": {
            "capabilities": {
                "experimentalApi": true,
                "mcpServerOpenaiFormElicitation": true,
                "extensions": { "openai/form": {} }
            },
            "clientInfo": {
                "name": "codex_thread_studio",
                "title": "Codex Thread Studio",
                "version": env!("CARGO_PKG_VERSION")
            }
        }
    }).to_string(),
).await
```
and the reply handling at `codex_app_server.rs:172-195`, which fires `initialized` only after the
matching id comes back:
```rust
if value.get("id").and_then(Value::as_i64) == Some(INITIALIZE_REQUEST_ID) {
    if value.get("error").is_some() { ...mark_stopped("initialization failed")... }
    let initialized = json!({ "method": "initialized", "params": {} }).to_string();
    if reader_input.send(initialized).await.is_err() { ... }
    reader_server.mark_ready(generation, value.get("result").cloned()...).await;
```

### The same sequence in TypeScript

`JaminZhou/codex-app-server-client/src/app-server-client.ts:733-747`:
```ts
const initializeParams = {
  capabilities: { ...DEFAULT_CAPABILITIES, ...this.options.capabilities },
  clientInfo: this.options.clientInfo ?? DEFAULT_CLIENT_INFO,
};
this.protocolValidator?.assertClientRequest("initialize", initializeParams);
const rawResponse = await peer.request<unknown>("initialize", initializeParams, this.withDefaultTimeout({}));
this.protocolValidator?.assertResponse("initialize", rawResponse);
const response = validateInitializeResponse(rawResponse);
this.protocolValidator?.assertClientNotification("initialized", undefined);
await peer.notify("initialized");
this.initializeResponse = response;
```

### And in Rust over WebSocket

`luisjpf/codex-app-server-client-cli/src/client/connection.rs:667-742`: builds `initialize` with
`id: 1`, sends, blocks for the matching id, stores `codex_home` / `platform_family` / `platform_os`
/ `user_agent` into `ServerMetadata`, sets `next_request_id = 2`, then sends `initialized` with
`params: None`. It hardcodes `experimental_api: true` and always supplies `ClientInfo.title`, which
the schema marks optional.

`GitHoobar/codex-app-server-client/src/client.rs:170-222` does the same with `initialize` at id 0
and buffers any notifications that arrive before the response, which is the correct defensive move:
the server can start emitting notifications immediately.

---

## 2. Framing

**Newline-delimited JSON over stdio. No `Content-Length` headers anywhere.** Proven upstream at
`codex-rs/app-server-transport/src/transport/stdio.rs:45-46` (`BufReader::new(stdin).lines()`) and
`:88-89` (`json.push('\n'); stdout.write_all(json.as_bytes())`).

Client-side proof, `JaminZhou/codex-app-server-client/src/jsonl-rpc-peer.ts:328` and `:341`:
```ts
this.reader = createInterface({ input, crlfDelay: Infinity });
...
this.output.write(`${message}\n`, "utf8", (error?: Error | null) => {
```

`paras-anekantvad/codex-app-server-client-sdk/src/codex_app_server_client/protocol/transport.py:67`
and `:158`:
```python
line = json.dumps(payload, separators=(",", ":")) + "\n"
```
with reads at `:80` (`await self._proc.stdout.readline()`).

`GitHoobar/codex-app-server-client/src/transport.rs:169-170` (`stdin.write_all(&line); stdin.flush()`)
and `:210-211` (`BufReader::new(stdout)`, `for line in reader.lines()`). Its module doc at
`src/transport.rs:79-80` says it outright: "Spawns `codex app-server` and frames JSON-RPC messages
as JSONL on its stdin/stdout."

On the unix and ws transports the JSON is carried inside WebSocket **text frames**, one JSON
document per frame, no trailing newline needed (see 3 above). `codex app-server proxy` converts
between the two.

---

## 4. Server notification shapes, as the clients model them and as the schema defines them

Full list of 78 server notification methods extracted from `schema/ServerNotification.json`. The
ones asked about:

### `thread/status/changed`

`schema/v2/ThreadStatusChangedNotification.json`, both `threadId` and `status` required:

```jsonc
{
  "method": "thread/status/changed",
  "params": {
    "threadId": "<id>",
    "status": {"type": "active", "activeFlags": ["waitingOnApproval"]}
  },
  "emittedAtMs": 1234567890123
}
```

`ThreadStatus` is a tagged `oneOf` on `type` with exactly four variants:
- `{"type": "notLoaded"}`
- `{"type": "idle"}`
- `{"type": "systemError"}`
- `{"type": "active", "activeFlags": [...]}` where `activeFlags` is **required** on this variant

`ThreadActiveFlag` is an enum with exactly two members:
```json
{"enum": ["waitingOnApproval", "waitingOnUserInput"]}
```
`activeFlags` is a plain array, so it can be `[]` (busy but not blocked), `["waitingOnApproval"]`,
`["waitingOnUserInput"]`, or both together. Treat non-empty `activeFlags` as "the human is being
asked something."

This is the single cheapest signal for your device: subscribe, watch for
`status.type === "active" && status.activeFlags.length > 0`, and the corresponding
`item/*/requestApproval` server request will be in flight on the same connection.

`messon007` treats this as a first-class allowlisted method
(`src-tauri/src/codex_app_server.rs:452`, grouped with `turn/started` and `turn/completed`, and
again in its test list at `:565` alongside `thread/tokenUsage/updated` at `:577`). None of the six
clients model `ThreadActiveFlag` as a typed enum; they pass the status through as opaque JSON.

### `turn/started` and `turn/completed`

Both are `{threadId, turn}`, both fields required
(`schema/v2/TurnStartedNotification.json`, `schema/v2/TurnCompletedNotification.json`). The `Turn`
object is large: `TurnStartedNotification.json` alone is 54 KB of `$defs` covering `CodexErrorInfo`
(with variants `contextWindowExceeded`, `usageLimitExceeded`, `rateLimitExceeded`,
`serverOverloaded`, `cyberPolicy`, `unauthorized`, `sandboxError`, `httpConnectionFailed`,
`responseStreamDisconnected`, `activeTurnNotSteerable`, and more), `CollabAgentState`,
`AsyncUserInputQuestion`, and the full item union.

`JaminZhou` shows the practical shape in `README.md:266-268`:
```ts
client.onNotification("turn/completed", ({ threadId, turn }) => {
  console.log(threadId, turn.status);
});
```
`turn.status` is what you want; do not try to model the whole `Turn`.

### `thread/tokenUsage/updated`

`schema/v2/ThreadTokenUsageUpdatedNotification.json`, all three of `threadId`, `turnId`,
`tokenUsage` required:

```jsonc
{
  "method": "thread/tokenUsage/updated",
  "params": {
    "threadId": "...",
    "turnId": "...",
    "tokenUsage": {
      "last":  {"inputTokens": 0, "cachedInputTokens": 0, "outputTokens": 0,
                "reasoningOutputTokens": 0, "totalTokens": 0, "cacheWriteInputTokens": 0},
      "total": {"...same six..."},
      "modelContextWindow": 272000
    }
  }
}
```
`ThreadTokenUsage` requires `last` and `total`; `modelContextWindow` is nullable int64.
`TokenUsageBreakdown` requires `cachedInputTokens`, `inputTokens`, `outputTokens`,
`reasoningOutputTokens`, `totalTokens`; `cacheWriteInputTokens` defaults to 0.

`total.totalTokens / modelContextWindow` is your context-pressure gauge. This is also the single
noisiest notification, and `pchalasani` opts out of it by name
(`claude_code_tools/codex_app_server_rpc.py`, the four-method opt-out list).

### The approval requests (params side)

These are **requests**, not notifications: they carry an `id` and you must answer them (see item 5).

`item/commandExecution/requestApproval`, `schema/CommandExecutionRequestApprovalParams.json`,
required `["itemId", "startedAtMs", "threadId", "turnId"]`:

```jsonc
{
  "id": 42,
  "method": "item/commandExecution/requestApproval",
  "params": {
    "threadId": "...", "turnId": "...", "itemId": "...",
    "startedAtMs": 1234567890123,
    "approvalId": null,           // non-null only for zsh-exec-bridge subcommand approvals
    "command": "npm test",
    "commandActions": [...],      // best-effort parsed, for friendly display
    "cwd": "/abs/path",
    "environmentId": null,
    "kind": null,                 // defaults to "command" on older servers
    "reason": "request for network access",
    "networkApprovalContext": {...},
    "proposedExecpolicyAmendment": [...],
    "proposedNetworkPolicyAmendments": [...]
  }
}
```
The last three are what let you offer "allow this host from now on" style buttons; feed them back in
the object decision variants from 5.2.

`item/fileChange/requestApproval`, `schema/FileChangeRequestApprovalParams.json`, required
`["itemId", "startedAtMs", "threadId", "turnId"]`, optional `reason` and `grantRoot` (the latter
marked `[UNSTABLE] ... unclear if this is honored today`). Note the params do **not** carry the
diff; get it from the matching `item/started` / `item/completed` notification or
`item/fileChange/patchUpdated`.

`item/permissions/requestApproval`, `schema/PermissionsRequestApprovalParams.json`, required
`["cwd", "itemId", "permissions", "startedAtMs", "threadId", "turnId"]`, where `permissions` is a
`RequestPermissionProfile` describing what is being asked for. Optional `environmentId`, `reason`.

`paras-anekantvad` models the first two in
`src/codex_app_server_client/types/approvals.py:17-29` and `:52-59` (missing `startedAtMs` on both,
and `proposed_execpolicy_amendment` typed as a dict when the schema says array). It does not model
the permissions request at all.

### Related notifications worth wiring up

- `serverRequest/resolved` -> `{requestId, threadId}`. Fires when *any* subscriber answered a server
  request. Use it to dismiss a prompt you were showing (see 5.5).
- `thread/started`, `thread/closed`, `thread/archived`, `thread/deleted`, `thread/name/updated`.
  `messon007` handles these at `ui/active-codex-connection.mjs:160, 167, 175-192, 217`.
- `item/started` / `item/completed` -> `{threadId, turnId, item, startedAtMs}`
  (`schema/v2/ItemStartedNotification.json`). The `item` is the full `ThreadItem` union.
- Delta streams to opt out of on a low-power device: `item/agentMessage/delta`,
  `item/reasoning/textDelta`, `item/reasoning/summaryTextDelta`,
  `item/reasoning/summaryPartAdded`, `item/commandExecution/outputDelta`,
  `item/fileChange/outputDelta`, `item/plan/delta`, `command/exec/outputDelta`,
  `process/outputDelta`, and the whole `thread/realtime/*` family.
- `error` and `warning` are top-level notification methods, distinct from JSON-RPC error responses.

---

## 7. Licenses, size, vendorability

| Repo | Language | License | Repo size | Core client size | Vendorable into one zero-dep Node file? |
|---|---|---|---|---|---|
| JaminZhou/codex-app-server-client | TypeScript | MIT (`license.spdx_id` = MIT) | 1320 KB | `src/app-server-client.ts` 38.8 KB / 1091 lines, `src/jsonl-rpc-peer.ts` 23.1 KB / 697 lines, `src/websocket-transport.ts` 15.2 KB | Not as-is. Ships 815 KB + 706 KB schema bundles and a 140 KB generated `src/generated/protocol/index.ts`. The **transport half is** though: `jsonl-rpc-peer.ts` alone is a clean, dependency-free JSONL JSON-RPC peer and is the single best thing to lift. |
| luisjpf/codex-app-server-client-cli | Rust | MIT (LICENSE present) | 160 KB | `src/client/connection.rs` 26.6 KB / 777 lines | No (Rust, and its approval payload is wrong). Read `docs/v1-architecture.md` and `skills/.../approvals.md` for UX ideas only. |
| GitHoobar/codex-app-server-client | Rust | **README says MIT but there is no LICENSE file and the GitHub API reports `license: null`** | 27 KB | `src/client.rs` 25.8 KB / 719 lines, `src/transport.rs` 13.2 KB / 393 lines | No (Rust). Smallest and cleanest of the Rust ones, good reading. Do not vendor code from it without asking the author to add a LICENSE file. |
| financialvice/codex-app-server-client | Swift | MIT (LICENSE present) | 335 KB | `CodexClient.swift` 28 KB, `CodexConnection.swift` 14.4 KB | No (Swift). But `Sources/.../CodexAppServerClient.docc/` is the best written prose in the whole survey: `HandlingApprovals.md`, `RoutingMultipleThreads.md`, `ErrorHandlingAndReconnect.md`, `CancellingATurn.md`. Read those. |
| paras-anekantvad/codex-app-server-client-sdk | Python | MIT (LICENSE present) | 84 KB | `_sync_client.py` 24.4 KB / 563 lines, `protocol/transport.py` 7.6 KB / 210 lines, `protocol/jsonrpc.py` 2.6 KB / 93 lines | No (Python). Smallest total surface of any full client. `protocol/jsonrpc.py` at 93 lines is a good size target for the equivalent Node module. Its approval types are wrong (see 5.4). |
| messon007/codex-thread-studio | JS + Rust (Tauri) | MIT (LICENSE present) | 10.6 MB | `src-tauri/src/codex_app_server.rs` 21.4 KB / 611 lines, `ui/active-codex-connection.mjs` 18.9 KB | No, it is a whole desktop app with 6 MB of vendored UI libs. But `src-tauri/src/codex_app_server.rs` is the most readable end-to-end spawn plus handshake plus multiplex implementation in the survey, and its notification allowlist at `:452` and `:560-580` is a ready-made "what actually matters" list. |

### Verdict for a zero-dependency Node file

Nothing here should be vendored wholesale. The honest assessment is that the whole client is small
enough to write from the schema:

- JSONL JSON-RPC peer with pending-request map: about 120 lines. Model on
  `JaminZhou/src/jsonl-rpc-peer.ts` (MIT) or `paras/protocol/jsonrpc.py` (93 lines, MIT).
- Spawn `codex app-server proxy` and do the `initialize` / `initialized` dance: about 30 lines.
- Approval router for the three `requestApproval` methods plus a `-32601` default: about 40 lines.
- Status tracker keyed on `thread/status/changed.activeFlags` plus `serverRequest/resolved`: about
  50 lines.

Total under 250 lines with no npm dependencies, provided you go through `codex app-server proxy`
rather than implementing RFC 6455.

---

## 8. Other clients found in the GitHub sweep, worth a look

From `gh search repos "codex app-server"` and `gh search code`. Not read in depth except where cited
above.

Observer or monitor shaped, which is closest to the device use case:
- `lucianlamp/codex-monitor` (Rust, 13 stars). Local-first monitor for app-server events. Best
  reference for endpoint discovery and the do-not-resume-unloaded-threads rule.
- `mideco-tech/codex-tg` (Go, 29 stars). "Telegram remote UI and observer." Spawns its own server
  (`internal/appserver/client.go:778`), and its `docs/adr/ADR-002-observer-delivery.md` documents
  that foreign-thread approvals degrade to waiting-state notifications when no actionable
  `request_id` exists.
- `openclaw/openclaw` (`extensions/codex/`). Has a written supervision spec with the
  passive-catalog rules and the broadcast/first-response-wins statement.
- `pchalasani/claude-code-tools` (Python + TS). Attach-only, both languages, with the explicit
  "the TUI is another subscriber and owns approvals" comment.
- `unstableneutron/pi-toolbox` (`extensions/pi-codex-app-server-use/`). Attach-only, daemon
  required, documents the suppression behavior when the socket is missing.

Other transports and SDKs:
- `pmenglund/codex-sdk-go`, `Zealbase/codex-app-server-go` (Go SDKs).
- `pablof7z/ai-sdk-provider-codex-app-server`, `janole/ai-sdk-provider-codex-asp` (Vercel AI SDK
  providers).
- `agentclientprotocol/codex-acp` -> `src/CodexAppServerClient.ts` (ACP bridge, TypeScript).
- `pwrdrvr/openclaw-codex-app-server` -> `src/client.ts` (267 stars).
- `getpaseo/paseo` -> `packages/server/src/server/agent/providers/codex-app-server-agent.ts`.
- `wieslawsoltes/CodexGui` (Avalonia desktop), `rebornix/Agmente` (Swift/iOS, 542 stars),
  `hebo6/codex-android` (Kotlin), `acking-you/pocket-codex` (Dart).
- `KKarsyline/codex-app-server-gateway-guide` is a written tutorial on streaming events, thread
  lifecycle and multi-backend routing (Chinese).

Mirrors of the upstream README that no longer exists in `openai/codex@main`:
`openinterpreter/openinterpreter`, `deonmenezes/mantishack`, both at `codex-rs/app-server/README.md`.
Useful if you want the older prose description of the protocol.

Forks of the protocol crate worth diffing for future changes: `limecloud/lime`
(`lime-rs/crates/app-server-protocol/src/protocol/v2/methods.rs` and `envelopes.rs`) carries a
readable enumeration of the v2 method set.

---

## 9. Cross-check summary: where third-party clients disagree with the schema

| Claim | Source | Schema says | Verdict |
|---|---|---|---|
| Approval reply is `{approved, decision: "approved"\|"denied", resume, resumeToken, approvalId}` | luisjpf `src/approval.rs:261-269` | `{"decision": "accept"\|"acceptForSession"\|"decline"\|"cancel"\|<object>}` for `item/commandExecution/requestApproval` | **Wrong.** Do not copy. |
| `CommandExecutionApprovalResponse` has an `acceptSettings` field | paras `types/approvals.py:44` | No such property | **Wrong**, extra field. |
| Approval decisions are only `accept`/`decline`/`cancel` | paras `types/approvals.py:32-37` | Also `acceptForSession`, `acceptWithExecpolicyAmendment`, `applyNetworkPolicyAmendment` | **Incomplete.** |
| `FileChangeApprovalRequest` has no `startedAtMs` | paras `types/approvals.py:52-59` | `startedAtMs` is required | **Incomplete.** |
| Legacy deny is the bare string `denied` | financialvice `ApprovalDecision.swift:12` (doc table) | `{"denied": {"rejection": "<string>"}}` object variant | **Check the encoder.** The doc table is misleading even if the `Codable` impl is right. |
| `ClientInfo.title` is required | luisjpf `connection.rs:672-676` (always sends it) | Only `name` and `version` required | Harmless, but not required. |
| Messages need `"jsonrpc": "2.0"` | luisjpf `connection.rs:668`, GitHoobar `client.rs:173-176` | Field is not in `JSONRPCRequest`/`Response`/`Notification` schemas, and the server omits it (`transport/mod.rs:353-357`) | Tolerated, not required. messon007 omits it and works. |
| There is a `thread/subscribe` method | none of them claim this | `ClientRequest` has `thread/unsubscribe` but no subscribe | Subscription is a side effect of `thread/start` and `thread/resume`. |
| Approvals fire on any sandbox | financialvice `HandlingApprovals.md:33-39` warns they do **not** on the unified-exec path | Not expressible in the schema | Worth verifying on 0.153.4 with a live session before shipping. `AskForApproval` in `schema/v2/ThreadStartParams.json` now has a `granular` object variant with `sandbox_approval`, `mcp_elicitations`, `rules`, `request_permissions`, `skill_approval`, which did not exist when that warning was written. |

## 10. Things this survey could not settle here

- Whether a plain `codex` TUI session registers its thread with the shared control-socket server.
  Not provable on this box: `codex app-server daemon start` does not work on this install
  (no standalone daemon, `~/.codex/app-server-control/` absent, both files in
  `~/.codex/app-server-daemon/` are zero bytes). Every observer client in the survey assumes yes.
- Whether the unified-exec approval suppression documented by financialvice still applies at
  0.153.4, and whether the new `granular` `AskForApproval` variant is the fix. Needs a live test.
- The exact `RequestPermissionProfile` shape carried in `item/permissions/requestApproval` params
  was not expanded here; it is in `schema/PermissionsRequestApprovalParams.json` definitions if
  needed.
