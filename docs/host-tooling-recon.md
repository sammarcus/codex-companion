# Host-side tooling recon: codex-companion npm package

Recon for a Node.js npm package installed via one `npx` command on macOS
(Linux/Windows best-effort). All findings below were verified with real
commands against the actual npm registry and a scratch install, on this
machine, on 2026-09-07. Anything not verifiable here (no Linux/Windows box,
no ESP32-S3 board plugged in) is explicitly labeled UNCONFIRMED.

Verification machine: macOS 26.6.2 (Darwin kernel 25.6.0, arm64), Node
v26.7.0, npm 11.19.0. Scratch install at
`/private/tmp/claude-501/-Users-sam-claude-codex-buddy/01d4d4d3-57c4-4db0-9b6b-9d7df0da23b4/scratchpad/npm-probe`.

## 1. npm package name availability

Checked with `npm view <name>` against the live registry:

| name | result |
|---|---|
| `codex-companion` | **404, free** (`npm error 404 Not Found`) |
| `codex-buddy` | **404, but not simply free** - registry says `Unpublished on 2026-04-02T21:31:35.011Z`. A name that has been unpublished is not guaranteed republishable; npm's abuse-prevention policy can permanently reserve names unpublished shortly after their first publish. Treat this name as risky, not confirmed-available. |
| `codex-desk` | **404, free** |
| `codex-desk-buddy` | **404, free** |
| `codex-vitals` | **404, free** |
| `codex-ring` | **404, free** |

Recommendation: use `codex-companion` (clean 404, no prior history) rather
than `codex-buddy`.

## 2. serialport

- Latest published version (npm dist-tags, verified via `npm view serialport dist-tags`): `{ beta: '10.2.2', latest: '13.0.0' }`. `beta` is actually an *older* major (10.2.2) still tagged beta; `latest` (13.0.0) is what a plain `npm install serialport` pulls.
- Installed into the scratch project: `serialport@13.0.0`, pulling in `@serialport/bindings-cpp@13.0.0` (native binding), `@serialport/stream@13.0.0`, and all the parser packages (`@serialport/parser-readline`, `-delimiter`, `-regex`, etc.) as direct dependencies of the `serialport` metapackage - no extra install needed for `ReadlineParser`.
- `engines.node` on `serialport`: `>=20.0.0`. `engines.node` on `@serialport/bindings-cpp`: `>=18.0.0`.

### Listing ports (vendorId/productId)

`SerialPort.list()` is a static async method (confirmed `typeof SerialPort.list === 'function'`, called it live). Ran on this machine with two real USB-serial devices attached and it returned:

```js
[
  { path: '/dev/tty.debug-console' },
  { path: '/dev/tty.Bluetooth-Incoming-Port' },
  { path: '/dev/tty.muffins' },
  {
    path: '/dev/tty.usbmodem0054452',
    manufacturer: 'ChargerLab',
    serialNumber: '005445',
    locationId: '02144000',
    vendorId: '5fc9',
    productId: '0063'
  },
  {
    path: '/dev/tty.usbmodem214501',
    manufacturer: 'Seeed Studio',
    serialNumber: '4EC8B549708DD063',
    locationId: '02145000',
    vendorId: '239a',
    productId: '8029'
  }
]
```

So the real shape per port (from `@serialport/bindings-interface`'s type
declarations, `node_modules/@serialport/bindings-interface/dist/*.d.ts`):

```ts
{
  path: string
  manufacturer: string | undefined
  serialNumber: string | undefined
  pnpId: string | undefined
  locationId: string | undefined
  productId: string | undefined
  vendorId: string | undefined
}
```

`vendorId`/`productId` are lowercase hex strings with no `0x` prefix (e.g.
`'303a'`, not `'0x303A'` and not `'303A'`). Non-USB ports (Bluetooth,
debug console) simply omit the USB fields entirely - `undefined`, not
empty string. Filter with something like:

```js
const ports = await SerialPort.list();
const candidates = ports.filter(p => p.vendorId?.toLowerCase() === '303a');
```

### Opening at 115200

Constructor signature (from `@serialport/stream`'s `.d.ts`, confirmed via
`grep` of the installed package):

```ts
new SerialPort({ path: '/dev/tty.usbmodemXXXX', baudRate: 115200 }, openCallback?)
```

`path` and `baudRate` are the only two required `OpenOptions` fields.
`autoOpen` defaults to `true` (port opens on next tick unless you pass
`autoOpen: false`).

### ReadlineParser

`ReadlineParser` is exported directly from the top-level `serialport`
package (confirmed `Object.keys(require('serialport'))` includes it
alongside `ByteLengthParser`, `DelimiterParser`, `RegexParser`, etc., plus
`SerialPort` and `SerialPortMock`). Standard usage:

```js
const { SerialPort, ReadlineParser } = require('serialport');
const port = new SerialPort({ path, baudRate: 115200 });
const parser = port.pipe(new ReadlineParser({ delimiter: '\n' }));
parser.on('data', line => { /* ... */ });
```

### Reconnect behaviour on unplug

Verified by reading `@serialport/stream/dist/index.js` (the class
`SerialPort` extends directly - confirmed via `serialport/dist/serialport.js`:
`class SerialPort extends stream_1.SerialPortStream`).

- On a read/write error from the OS-level binding, the stream calls an
  internal `_disconnected(err)`, which calls `this.close(undefined, new
  DisconnectedError(err.message))`.
- That `close()` call does `this.emit('close', disconnectError)`.
- So **the event that fires on unplug is `'close'`**, not a distinct
  `'disconnect'` event. The callback receives an error object as its
  argument, and that error has `.disconnected === true` (from the
  `DisconnectedError` class: `constructor(message) { super(message);
  this.disconnected = true; }`).
- There is no built-in auto-reconnect. A caller that wants reconnect
  behaviour has to listen for `'close'`, check `err?.disconnected`, and
  re-poll `SerialPort.list()` / re-construct a new `SerialPort` instance on
  a timer (the old instance's binding is dead once disconnected; you can't
  reopen the same object after a physical unplug in a defined way across
  platforms).

```js
port.on('close', err => {
  if (err?.disconnected) {
    // physical unplug - start polling SerialPort.list() to reconnect
  }
});
```

### Native compilation on install

- `@serialport/bindings-cpp` ships prebuilt native bindings for essentially
  every common target, bundled inside the package tarball itself (not as
  separate optional platform packages - confirmed `npm view serialport
  optionalDependencies` returned empty, and there's a single
  `@serialport/bindings-cpp` dependency).
- Verified by inspecting the installed package's `prebuilds/` directory,
  which is 2.0 MB total (whole scratch `node_modules` is 3.2 MB) and
  contains:
  - `darwin-x64+arm64/@serialport+bindings-cpp.node` - **one universal
    (fat) Mach-O binary** covering both Intel and Apple Silicon Macs.
    Confirmed with `lipo -info`:
    `Architectures in the fat file: ... x86_64 arm64`.
  - `linux-x64` (glibc + musl variants), `linux-arm64` (glibc + musl),
    `linux-arm` (armv6/armv7, glibc + musl)
  - `win32-x64`, `win32-ia32`, `win32-arm64`
  - `android-arm`, `android-arm64`
- The package's `binary.napi_versions` is `[8]` and it builds against
  **N-API**, not the raw V8/`NODE_MODULE_VERSION` ABI. N-API is
  ABI-stable *forward*: a binding built for N-API version 8 loads fine on
  any Node runtime whose N-API version is >= 8. This Node v26.7.0's
  reported `process.versions.napi` is `10`, so the bundled prebuild loaded
  and ran with zero compilation - confirmed by actually calling
  `SerialPort.list()` immediately after `npm install` with no build step,
  no `node-gyp`, no Xcode toolchain invoked.
- One real gotcha hit during the install: npm 11.19's install-scripts
  allowlist (`npm config get install-links` = `false` doesn't govern this;
  it's the newer "install scripts not covered by allowScripts" gate) printed:
  ```
  npm warn install-scripts 1 package has install scripts not yet covered by allowScripts:
  npm warn install-scripts   @serialport/bindings-cpp@13.0.0 (install: node-gyp-build)
  ```
  This means the package's `"install": "node-gyp-build"` lifecycle script
  did **not** run. It didn't matter: `node-gyp-build` is also invoked
  lazily at `require()` time (via `dist/load-bindings.js` ->
  `serialport-bindings.js`, which resolves the platform prebuild on load,
  not only at install time), so the module still worked. Worth documenting
  for recipients on newer npm - they may see this warning and think the
  install failed; it didn't. If a recipient is on an npm/config that
  actually blocks `require()`-time resolution too (unlikely, but some
  hardened CI setups strip `prebuilds/` at install), the fallback is:
  `npm install --foreground-scripts` or `npm rebuild @serialport/bindings-cpp`
  which needs a C++ toolchain (Xcode CLT on macOS, `build-essential` on
  Linux, VS Build Tools on Windows).
- **This machine's Node v26.7.0 is supported.** Both `serialport`
  (`engines.node >= 20.0.0`) and `@serialport/bindings-cpp` (`engines.node
  >= 18.0.0`) declare minimums well below v26, and the N-API-8 prebuild
  loaded and ran without any fallback needed. Because the darwin prebuild
  is N-API-based rather than per-Node-version ABI-based, there's no
  separate "node 22 vs 24 vs 26" prebuild to check for darwin - one
  `darwin-x64+arm64` binary serves all of them. (Linux/Windows prebuilds
  are architecture/libc-split, not Node-version-split, for the same N-API
  reason.)

## 3. Node built-in serial API

**No.** Confirmed on this machine, Node v26.7.0:

```
$ node -e "console.log(require('node:module').builtinModules.filter(m=>/serial/i.test(m)))"
[]
$ node -e "require('node:serialport')"
ERR_UNKNOWN_BUILTIN_MODULE No such built-in module: node:serialport
```

There is no `node:serialport` or any other built-in serial-port module in
Node 26. A third-party package (`serialport`, as above) is required.

## 4. launchd LaunchAgent

Template written to
`/Users/sam/claude/codex-buddy/helper/templates/com.codex-companion.plist`
and validated:

```
$ plutil -lint helper/templates/com.codex-companion.plist
helper/templates/com.codex-companion.plist: OK
```

It's a **template** - `__NODE_BIN__`, `__INSTALL_DIR__`, and `__HOME__` are
placeholders an installer script substitutes at install time (there's no
variable expansion in launchd plists; `$HOME` and `~` are not expanded).
A fully-substituted copy was also rendered and linted clean on this
machine (`node` resolved via `which node` -> `/opt/homebrew/bin/node`,
`$HOME` -> `/Users/sam`) to confirm the substitution produces valid XML.

Key settings, as required:
- `ProgramArguments`: `[node, <install-dir>/index.js, run]`
- `KeepAlive`: `true`
- `RunAtLoad`: `true`
- `StandardOutPath` / `StandardErrorPath`: `~/Library/Logs/codex-companion.log` (both streams to the same file)
- `ProcessType`: `Background` (not `Interactive` - no UI, don't get killed under memory pressure the way foreground apps do)
- `EnvironmentVariables.PATH`: launchd agents run with a minimal environment, not the user's shell PATH/rc files, so PATH is set explicitly

### npx cache path gotcha

`npx <package>` runs the package straight out of npm's on-demand cache
(`~/.npm/_npx/<hash>/node_modules/...` on macOS) and **that cache entry is
not stable** - it can be evicted, and the hashed path differs machine to
machine and can differ run to run. Pointing a LaunchAgent's
`ProgramArguments` at an `_npx` cache path will eventually break (service
fails silently or points at deleted files after a cache prune).

The plist must point at a **stable, persistent** install location. Two
options, in order of recommendation:

1. **`npm install -g codex-companion`** - resolves to a stable global
   `node_modules` path (e.g. `$(npm root -g)/codex-companion`). Simplest,
   but requires the recipient's global npm prefix to be writable without
   `sudo` (true for nvm/Homebrew-node setups, not always true for the
   system Node on macOS).
2. **Copy the package into `~/.codex-companion/`** - the installer (the
   thing the recipient runs via `npx codex-companion install` or similar)
   copies its own resolved `node_modules` tree into a fixed per-user
   directory, then writes the plist pointing at
   `~/.codex-companion/index.js`. More resilient across npm/global-prefix
   differences, and matches the placeholder convention used in the
   template above (`__INSTALL_DIR__` defaults to this path).

Either way, resolve `node`'s absolute path (`which node` or
`process.execPath` from inside the installer, which is running under the
same Node the recipient already has) and bake that absolute path into
`ProgramArguments[0]` rather than relying on `env node` - launchd's
minimal `PATH` may not include an nvm shim or Homebrew's bin dir.

### launchctl commands for this macOS (Darwin 25 / macOS 26.x)

Modern launchctl (post-10.10) domain/target syntax, verified against this
machine's `launchctl` (confirmed via `man launchctl` and a live
`launchctl print-disabled gui/$(id -u)` call, uid 501 on this machine):

```bash
# install / start
cp helper/templates/com.codex-companion.plist ~/Library/LaunchAgents/com.codex-companion.plist
# (after substituting __NODE_BIN__ / __INSTALL_DIR__ / __HOME__)
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.codex-companion.plist

# stop / uninstall
launchctl bootout gui/$(id -u)/com.codex-companion

# check status
launchctl print gui/$(id -u)/com.codex-companion

# restart after editing the plist (bootout, then bootstrap again)
launchctl bootout gui/$(id -u)/com.codex-companion 2>/dev/null
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.codex-companion.plist
```

Do **not** use the old `launchctl load`/`unload` - deprecated since
10.10 (this machine is macOS 26.6.2 / Darwin 25.6.0) in favor of
`bootstrap`/`bootout` against an explicit domain target. `gui/<uid>` is
the correct domain for a per-user, per-GUI-session LaunchAgent (as opposed
to `system/` for a root LaunchDaemon).

## 5. Linux / Windows notes

Not independently verified on this machine (no Linux/Windows box
available in this environment) - general platform guidance, best-effort
per the task:

- **Linux**: serial device nodes (`/dev/ttyUSB0`, `/dev/ttyACM0`, etc.)
  are typically owned by root with group `dialout` (Debian/Ubuntu) or
  `uucp` (some other distros/Arch). A user not in that group gets
  `EACCES` on open even though `SerialPort.list()` can still enumerate
  the port. Fix: `sudo usermod -a -G dialout $USER` then log out/in (group
  membership is read at login). No standing udev rule is required for a
  generic CDC-ACM device to appear - the kernel's built-in `cdc_acm`
  driver creates the node automatically; a custom udev rule is only
  needed for a fixed/predictable symlink (e.g. matching on
  `ATTRS{idVendor}=="303a"` to create `/dev/codex-companion`) or to grant
  non-root permissions without group membership.
- Linux launchd-equivalent is out of scope here (systemd user units), but
  worth flagging since the task's autostart mechanism (`KeepAlive`,
  `RunAtLoad`) doesn't carry over - a Linux port of the installer would
  need a `systemd --user` unit instead of a plist.
- **Windows**: USB CDC devices need either the built-in `usbser.sys` class
  driver (Windows 10/11 do this automatically for a device presenting the
  standard CDC-ACM USB class, which is what Espressif's `ARDUINO_USB_CDC_ON_BOOT`
  native-USB mode presents) or a vendor `.inf`. No admin rights or manual
  driver install should be needed on Windows 10/11 for a straightforward
  CDC-ACM ESP32-S3 device. Node's `serialport` prebuilds cover
  `win32-x64`, `win32-ia32`, and `win32-arm64` (confirmed present in the
  installed package's `prebuilds/` directory). Autostart-on-login
  equivalent would be a Startup-folder shortcut or a registered Scheduled
  Task, not covered further here since it's out of scope for this pass.

## 6. ESP32-S3 native USB CDC VID/PID

**Partially confirmed, from a file in this repo, not from live hardware**
(the T-Display-S3 boards are not plugged in and were not flashed, per
instructions).

`vendor/T-Display-S3/boards/lilygo-t-display-s3.json` (PlatformIO board
definition checked into this repo) declares:

```json
"hwids": [
  [ "0X303A", "0x1001" ]
]
```

`0x303A` is Espressif Systems' USB vendor ID. `0x1001` is the PID this
board definition expects for its native USB (CDC-ACM) mode - this matches
Espressif's generic/default `USB_PID` for `ARDUINO_USB_CDC_ON_BOOT=1`
projects that don't override `usb_pid` in their build flags (confirmed
`platformio.ini` in this repo sets `-DARDUINO_USB_CDC_ON_BOOT=1` and
`-DARDUINO_USB_MODE=1` without a custom PID flag).

**UNCONFIRMED**: the exact string shape `SerialPort.list()` would report
for a live T-Display-S3, since no board is connected. Based on the live
macOS test above (real devices returned `vendorId: '239a'`, i.e. the
board's declared `0X303A`/`0x1001` would be expected to surface as
`vendorId: '303a'`, `productId: '1001'`, lowercase, no `0x` prefix) - but
this is inference from the field-formatting pattern observed on other
devices, not a confirmed observation of this specific board.

### Fallback: matching by manufacturer/product string instead of VID/PID

If a recipient's board reports a different PID (custom firmware, a
different `-DUSB_PID` build flag, a clone board with different hwids, or a
device that isn't in native-USB mode at all and instead uses a USB-UART
bridge chip like CP2102/CH340 - which would show *that chip's* VID/PID,
not Espressif's), matching on `vendorId === '303a'` alone is fragile.
`@serialport/bindings-interface`'s port-info type also exposes
`manufacturer` and `pnpId` (both `string | undefined`) which can be used
as a secondary/fallback match:

```js
const ports = await SerialPort.list();
const candidates = ports.filter(p =>
  p.vendorId?.toLowerCase() === '303a' ||               // Espressif native USB
  /espressif|esp32/i.test(p.manufacturer ?? '') ||       // string fallback
  /303a/i.test(p.pnpId ?? '')                            // pnpId fallback (not always present)
);
```

Recommend the installer present the full `SerialPort.list()` output to
the user for manual pick if auto-match finds zero or more than one
candidate, rather than silently guessing - `manufacturer` was `undefined`
even on some of this machine's real non-serial ports, and CDC-ACM
`manufacturer` strings are firmware-supplied and not guaranteed present or
consistent across board revisions.
