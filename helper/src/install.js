'use strict';

/**
 * install.js - subcommands: run, install, uninstall, status, demo, doctor.
 *
 * macOS is the supported target (launchd LaunchAgent). Linux gets a
 * best-effort `systemd --user` unit. Windows gets printed instructions.
 *
 * launchctl syntax is the modern domain/target form (bootstrap/bootout against
 * gui/<uid>), not the deprecated load/unload - see docs/host-tooling-recon.md.
 */

const fs = require('node:fs');
const fsp = require('node:fs/promises');
const path = require('node:path');
const os = require('node:os');
const { spawnSync } = require('node:child_process');

const { CodexWatcher, codexHome, newestSessionFile } = require('./codex-watcher');
const { buildFrame, framesEqual } = require('./frame');
const { DeviceLink, matchPorts } = require('./device');

const LABEL = 'com.codex-companion';
const PKG_ROOT = path.resolve(__dirname, '..');
const TEMPLATE_PATH = path.join(PKG_ROOT, 'templates', `${LABEL}.plist`);

function homeDir() {
  return process.env.HOME || os.homedir();
}

function installDir() {
  return path.join(homeDir(), '.codex-companion', 'app');
}

function plistPath() {
  return path.join(homeDir(), 'Library', 'LaunchAgents', `${LABEL}.plist`);
}

function logPath() {
  return path.join(homeDir(), 'Library', 'Logs', 'codex-companion.log');
}

function systemdUnitPath() {
  const base =
    process.env.XDG_CONFIG_HOME || path.join(homeDir(), '.config');
  return path.join(base, 'systemd', 'user', 'codex-companion.service');
}

/* ------------------------------------------------------------------ *
 * plist rendering (pure, so it can be tested without touching launchd)
 * ------------------------------------------------------------------ */

/**
 * Escape the three characters that would otherwise break the plist XML.
 * A home directory really can contain "&" (/Users/a&b), and plutil rejects a
 * bare ampersand as an unknown escape sequence.
 */
function xmlEscape(v) {
  return String(v).split('&').join('&amp;').split('<').join('&lt;').split('>').join('&gt;');
}

/**
 * Node paths that vanish on the next upgrade. Homebrew deletes the old Cellar
 * directory on `brew upgrade node`, and nvm/fnm/volta do the same on uninstall,
 * at which point launchd cannot spawn the job and the board silently freezes on
 * its last frame.
 */
function isVolatileNodeBin(p) {
  return (
    /\/Cellar\/node(@[\d.]+)?\//.test(p) ||
    p.includes('/.nvm/') ||
    p.includes('/.fnm/') ||
    p.includes('/.volta/') ||
    p.includes('/.asdf/')
  );
}

/** Version-stable aliases, in the order we prefer them. */
const NODE_ALIASES = ['/opt/homebrew/bin/node', '/usr/local/bin/node', '/usr/bin/node'];

/**
 * Prefer a stable alias that resolves to the very node we are running, so the
 * plist keeps working across `brew upgrade node`. Falls back to execPath when
 * no alias points at the same binary.
 */
function stableNodeBin(execPath, aliases) {
  let real;
  try {
    real = fs.realpathSync(execPath);
  } catch {
    real = execPath;
  }
  for (const alias of aliases || NODE_ALIASES) {
    if (alias === execPath) return alias;
    let r;
    try {
      r = fs.realpathSync(alias);
    } catch {
      continue;
    }
    if (r === real) return alias;
  }
  return execPath;
}

/**
 * Substitute the three placeholders in the LaunchAgent template.
 * There is no variable expansion in launchd plists: $HOME and ~ are literal,
 * so every path has to be absolute by the time it lands on disk.
 *
 * @param {string} template raw plist text
 * @param {{nodeBin:string,installDir:string,home:string}} vars
 */
function renderPlist(template, vars) {
  if (typeof template !== 'string' || !template) {
    throw new Error('renderPlist: template is empty');
  }
  for (const k of ['nodeBin', 'installDir', 'home']) {
    const v = vars && vars[k];
    if (typeof v !== 'string' || !v) throw new Error(`renderPlist: missing ${k}`);
    if (!path.isAbsolute(v)) throw new Error(`renderPlist: ${k} must be absolute, got ${v}`);
  }
  const out = template
    .split('__NODE_BIN__')
    .join(xmlEscape(vars.nodeBin))
    .split('__INSTALL_DIR__')
    .join(xmlEscape(vars.installDir))
    .split('__HOME__')
    .join(xmlEscape(vars.home));
  const leftover = out.match(/__[A-Z_]+__/);
  if (leftover) throw new Error(`renderPlist: unsubstituted placeholder ${leftover[0]}`);
  return out;
}

function renderSystemdUnit(vars) {
  return [
    '[Unit]',
    'Description=Codex Desk Companion (T-Display-S3)',
    'After=default.target',
    '',
    '[Service]',
    'Type=simple',
    `ExecStart=${vars.nodeBin} ${path.join(vars.installDir, 'index.js')} run`,
    `WorkingDirectory=${vars.installDir}`,
    'Restart=always',
    'RestartSec=5',
    '',
    '[Install]',
    'WantedBy=default.target',
    ''
  ].join('\n');
}

/* ------------------------------------------------------------------ *
 * small helpers
 * ------------------------------------------------------------------ */

function run(cmd, args, opts) {
  const r = spawnSync(cmd, args, Object.assign({ encoding: 'utf8' }, opts || {}));
  return {
    ok: r.status === 0,
    status: r.status,
    stdout: (r.stdout || '').trim(),
    stderr: (r.stderr || '').trim(),
    error: r.error
  };
}

function uid() {
  return typeof process.getuid === 'function' ? process.getuid() : 0;
}

function log(...a) {
  process.stdout.write(a.join(' ') + '\n');
}

/**
 * Flags that never take a value. Without this list, `demo --loop extra` would
 * read "extra" as the value of --loop and lose the positional argument.
 */
const BOOLEAN_FLAGS = new Set([
  'dry-run',
  'loop',
  'global',
  'no-heuristic',
  'help',
  'h',
  'version'
]);

/**
 * Flags a bare `codex-companion` (install + run) legitimately accepts. Anything
 * else means the user typed something we do not understand, and we must not
 * answer that by installing a background service.
 */
const DEFAULT_OK_FLAGS = new Set([
  'port',
  'vid',
  'pid',
  'dry-run',
  'global',
  'codex-home',
  'no-heuristic'
]);

function parseArgs(argv) {
  const out = { _: [], flags: {} };
  for (let i = 0; i < argv.length; i += 1) {
    const a = argv[i];
    if (a.startsWith('--')) {
      const eq = a.indexOf('=');
      if (eq !== -1) {
        out.flags[a.slice(2, eq)] = a.slice(eq + 1);
      } else if (BOOLEAN_FLAGS.has(a.slice(2))) {
        out.flags[a.slice(2)] = true;
      } else if (argv[i + 1] && !argv[i + 1].startsWith('-')) {
        out.flags[a.slice(2)] = argv[i + 1];
        i += 1;
      } else {
        out.flags[a.slice(2)] = true;
      }
    } else {
      out._.push(a);
    }
  }
  return out;
}

/** Locate the node_modules directory that actually holds serialport. */
function serialportTree() {
  let resolved;
  try {
    resolved = require.resolve('serialport');
  } catch {
    return null;
  }
  const marker = `${path.sep}node_modules${path.sep}`;
  const idx = resolved.lastIndexOf(`${marker}serialport${path.sep}`);
  if (idx === -1) return null;
  return resolved.slice(0, idx + marker.length - 1);
}

/**
 * Is this the error you get when `require('serialport')` cannot resolve?
 * A partially copied install, or a node upgrade that orphaned the prebuild.
 */
function isSerialportMissing(e) {
  const msg = (e && e.message) || '';
  const missing = (e && e.code === 'MODULE_NOT_FOUND') || /Cannot find module/.test(msg);
  return Boolean(missing && /serialport/.test(msg));
}

/**
 * Attach the 'error' handler that both `run` and `demo` need.
 *
 * DeviceLink retries findPort() every scanMs forever, so an undeduped handler
 * writes the same stack on every sweep: measured at 29 lines / 2283 bytes in
 * 10 s from a copy with no node_modules, i.e. ~19 MB/day appended to
 * ~/Library/Logs/codex-companion.log, which the plist never rotates and
 * KeepAlive never truncates. So: dedupe by message, exactly as the sibling
 * 'scan' handler already does, and treat an unloadable serialport as fatal.
 * It cannot resolve itself by being retried, and exiting hands the problem to
 * launchd's 10 s respawn throttle, which bounds it.
 */
function attachDeviceErrorLogging(link) {
  let lastErrorMessage = null;
  link.on('error', (e) => {
    const msg = (e && e.message) || String(e);
    if (isSerialportMissing(e)) {
      log(`[device] fatal: serialport could not be loaded: ${msg.split('\n')[0]}`);
      log('         this install is incomplete. Re-run: codex-companion install');
      link.stop();
      process.exit(1);
    }
    if (msg === lastErrorMessage) return;
    lastErrorMessage = msg;
    log(`[device] error: ${msg}`);
  });
}

/* ------------------------------------------------------------------ *
 * cmd: run
 * ------------------------------------------------------------------ */

async function cmdRun(args) {
  const opts = args.flags || {};
  const watcher = new CodexWatcher({
    codexHome: opts['codex-home'] ? String(opts['codex-home']) : undefined,
    approvalHeuristic: opts['no-heuristic'] ? false : true
  });

  const dry = Boolean(opts['dry-run']);
  let link = null;

  if (!dry) {
    link = new DeviceLink({
      port: typeof opts.port === 'string' ? opts.port : undefined,
      vendorId: typeof opts.vid === 'string' ? opts.vid : undefined,
      productId: typeof opts.pid === 'string' ? opts.pid : undefined
    });
    link.on('open', (p) => log(`[device] open ${p}`));
    link.on('hello', (l) => log(`[device] hello ${l === null ? '(timed out, proceeding)' : l}`));
    link.on('close', (e) =>
      log(`[device] closed${e && e.disconnected ? ' (unplugged)' : ''}${e ? `: ${e.message}` : ''}`)
    );
    attachDeviceErrorLogging(link);
    // Without this, a bad cable or a board in download mode is indistinguishable
    // from a working setup: DeviceLink emits nothing until a port opens.
    let lastScanReason = null;
    link.on('scan', (r) => {
      if (r && r.chosen) return;
      const reason = (r && r.reason) || 'no port matched';
      if (reason === lastScanReason) return;
      lastScanReason = reason;
      log(`[device] no board found yet: ${reason}`);
    });
    link.start();
  }

  watcher.on('file', (f) => log(`[codex] watching ${f || '(no session file yet)'}`));
  watcher.on('error', (e) => log(`[codex] error: ${e.message}`));
  // --dry-run has to be an honest preview, so it applies the same throttle,
  // dedupe and heartbeat rules the serial link would.
  let dryLast = null;
  let dryLastAt = -Infinity;
  watcher.on('tick', (snap) => {
    if (link) {
      link.update(snap);
      return;
    }
    const f = buildFrame(snap);
    const now = Date.now();
    const since = now - dryLastAt;
    if (since < 250) return;
    if (framesEqual(dryLast, f) && since < 2000) return;
    process.stdout.write(JSON.stringify(f) + '\n');
    dryLast = f;
    dryLastAt = now;
  });

  await watcher.start();
  log(`[codex] home ${watcher.codexHome}`);

  // Every timer inside CodexWatcher and DeviceLink is unref'd on purpose, and a
  // pending promise is not a libuv handle, so without one ref'd handle Node
  // drains the loop and exits 0 immediately. Under launchd (KeepAlive=true)
  // that becomes a respawn storm, which is exactly what a fresh recipient hits
  // before they have ever run codex and ~/.codex/sessions exists.
  const keepAlive = setInterval(() => {}, 60000);

  const shutdown = () => {
    clearInterval(keepAlive);
    watcher.stop();
    if (link) link.stop();
    process.exit(0);
  };
  process.on('SIGINT', shutdown);
  process.on('SIGTERM', shutdown);

  return new Promise(() => {});
}

/* ------------------------------------------------------------------ *
 * cmd: install
 * ------------------------------------------------------------------ */

async function copyPackage(dest) {
  await fsp.mkdir(dest, { recursive: true });
  // Re-running `install` from the installed copy is the obvious repair path a
  // recipient takes, and fs.cp throws ERR_FS_CP_EINVAL when src and dest are
  // the same. Skip those entries instead of dumping a stack trace at them.
  const selfInstall = path.resolve(PKG_ROOT) === path.resolve(dest);
  const entries = ['index.js', 'package.json', 'README.md', 'bin', 'src', 'templates'];
  for (const e of entries) {
    const from = path.join(PKG_ROOT, e);
    if (!fs.existsSync(from)) continue;
    const to = path.join(dest, e);
    if (path.resolve(from) === path.resolve(to)) continue;
    await fsp.cp(from, to, { recursive: true, force: true });
  }
  if (selfInstall) {
    return { deps: 'already installed here, nothing to copy' };
  }
  const tree = serialportTree();
  if (tree && path.resolve(tree) === path.resolve(path.join(dest, 'node_modules'))) {
    return { deps: 'already installed here, nothing to copy' };
  }
  if (tree) {
    // npx hoists deps a level above the package, so copy the whole
    // node_modules that actually contains serialport rather than ours.
    await fsp.cp(tree, path.join(dest, 'node_modules'), { recursive: true, force: true });
    return { deps: 'copied', from: tree };
  }
  const r = run('npm', ['install', '--omit=dev', '--no-audit', '--no-fund'], { cwd: dest });
  return { deps: r.ok ? 'npm install' : 'FAILED', npm: r };
}

async function cmdInstall(args) {
  const flags = args.flags || {};
  const nodeBin = stableNodeBin(process.execPath);
  if (isVolatileNodeBin(nodeBin)) {
    log(`warning: ${nodeBin} is a version-pinned path.`);
    log('         A node upgrade deletes it and the background service stops');
    log('         starting, with no visible error. Re-run `install` afterwards.');
  }
  const home = homeDir();
  let dir;

  if (flags.global) {
    // Never `npm install -g codex-companion`: the package is not published, so
    // installing it by name 404s at the registry every single time. Install the
    // artifact we are already running from instead. npm reads its package.json
    // for the name, so `npm uninstall -g codex-companion` still undoes it.
    const r = run('npm', ['install', '-g', PKG_ROOT]);
    if (!r.ok) {
      log(`npm install -g ${PKG_ROOT} failed:`);
      log(r.stderr || r.stdout);
      return 1;
    }
    const root = run('npm', ['root', '-g']);
    dir = path.join(root.stdout, 'codex-companion');
    log(`installed globally at ${dir}`);
  } else {
    dir = installDir();
    log(`copying package to ${dir}`);
    const res = await copyPackage(dir);
    log(`dependencies: ${res.deps}${res.from ? ` (from ${res.from})` : ''}`);
    if (res.deps === 'FAILED') {
      log(res.npm && res.npm.stderr ? res.npm.stderr : 'could not install serialport');
      return 1;
    }
  }

  if (process.platform === 'darwin') {
    return installLaunchAgent({ nodeBin, dir, home });
  }
  if (process.platform === 'linux') {
    return installSystemd({ nodeBin, dir, home });
  }
  printWindowsInstructions({ nodeBin, dir });
  return 0;
}

function installLaunchAgent(v) {
  const template = fs.readFileSync(TEMPLATE_PATH, 'utf8');
  const rendered = renderPlist(template, {
    nodeBin: v.nodeBin,
    installDir: v.dir,
    home: v.home
  });
  const target = plistPath();
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.mkdirSync(path.dirname(logPath()), { recursive: true });
  fs.writeFileSync(target, rendered, 'utf8');
  log(`wrote ${target}`);

  const lint = run('plutil', ['-lint', target]);
  if (!lint.ok) {
    log(`plutil -lint failed: ${lint.stderr || lint.stdout}`);
    // Never leave an invalid plist sitting in ~/Library/LaunchAgents.
    try {
      fs.rmSync(target);
      log(`removed the invalid ${target}`);
    } catch {
      /* nothing more we can do */
    }
    return 1;
  }

  if (process.env.CODEX_COMPANION_NO_LAUNCHCTL) {
    // Escape hatch for testing the install path without registering a service.
    log('CODEX_COMPANION_NO_LAUNCHCTL set: skipping launchctl bootstrap');
    log(`to start it by hand: launchctl bootstrap gui/${uid()} ${target}`);
    return 0;
  }

  // bootout first so a re-install replaces cleanly; a not-loaded service
  // makes bootout fail, which is fine.
  run('launchctl', ['bootout', `gui/${uid()}/${LABEL}`]);
  const boot = run('launchctl', ['bootstrap', `gui/${uid()}`, target]);
  if (!boot.ok) {
    log(`launchctl bootstrap failed (${boot.status}): ${boot.stderr || boot.stdout}`);
    return 1;
  }
  log(`launchctl bootstrap gui/${uid()} ok`);
  log(`logs: ${logPath()}`);
  return 0;
}

function installSystemd(v) {
  const unit = renderSystemdUnit({ nodeBin: v.nodeBin, installDir: v.dir });
  const target = systemdUnitPath();
  fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.writeFileSync(target, unit, 'utf8');
  log(`wrote ${target}`);
  if (process.env.CODEX_COMPANION_NO_LAUNCHCTL) {
    log('CODEX_COMPANION_NO_LAUNCHCTL set: skipping systemctl');
    return 0;
  }
  const reload = run('systemctl', ['--user', 'daemon-reload']);
  if (!reload.ok) {
    log('systemctl --user daemon-reload failed; start it manually:');
    log('  systemctl --user daemon-reload && systemctl --user enable --now codex-companion');
    return 0;
  }
  const en = run('systemctl', ['--user', 'enable', '--now', 'codex-companion']);
  if (!en.ok) {
    log(`systemctl --user enable --now failed: ${en.stderr || en.stdout}`);
    return 1;
  }
  log('systemd user service enabled and started');
  log('If the port opens with EACCES, add yourself to the dialout group:');
  log('  sudo usermod -a -G dialout "$USER"   # then log out and back in');
  return 0;
}

function printWindowsInstructions(v) {
  log('Windows autostart is not automated. Do this once:');
  log('  1. Press Win+R, type  shell:startup  and press Enter.');
  log('  2. Create a shortcut in that folder pointing at:');
  log(`       "${v.nodeBin}" "${path.join(v.dir, 'index.js')}" run`);
  log('  3. Sign out and back in, or double-click the shortcut to start now.');
  log('');
  log('Windows 10/11 bind the built-in usbser.sys CDC driver automatically;');
  log('no driver install should be needed. The board shows up as a COM port.');
}

/* ------------------------------------------------------------------ *
 * cmd: uninstall
 * ------------------------------------------------------------------ */

async function cmdUninstall() {
  // Read the plist before removing it: a --global install points somewhere
  // outside ~/.codex-companion, and deleting that directory would not undo it.
  let globalDir = null;
  if (process.platform === 'darwin' && fs.existsSync(plistPath())) {
    try {
      const text = fs.readFileSync(plistPath(), 'utf8');
      const m = /<string>([^<]*index\.js)<\/string>/.exec(text);
      if (m && !path.resolve(m[1]).startsWith(path.resolve(path.dirname(installDir())))) {
        globalDir = path.dirname(m[1]);
      }
    } catch {
      /* unreadable plist: fall through to the normal path */
    }
  }

  if (process.platform === 'darwin') {
    const boot = run('launchctl', ['bootout', `gui/${uid()}/${LABEL}`]);
    log(boot.ok ? 'launchctl bootout ok' : 'service was not loaded');
    const target = plistPath();
    if (fs.existsSync(target)) {
      fs.rmSync(target);
      log(`removed ${target}`);
    }
  } else if (process.platform === 'linux') {
    run('systemctl', ['--user', 'disable', '--now', 'codex-companion']);
    const target = systemdUnitPath();
    if (fs.existsSync(target)) {
      fs.rmSync(target);
      log(`removed ${target}`);
    }
    run('systemctl', ['--user', 'daemon-reload']);
  } else {
    log('Remove the shortcut from  shell:startup  to stop autostart.');
  }

  const dir = path.dirname(installDir()); // ~/.codex-companion
  if (fs.existsSync(dir)) {
    fs.rmSync(dir, { recursive: true, force: true });
    log(`removed ${dir}`);
  }

  // The log only exists because the service wrote to it.
  if (fs.existsSync(logPath())) {
    fs.rmSync(logPath(), { force: true });
    log(`removed ${logPath()}`);
  }

  if (globalDir) {
    log(`this was a --global install (${globalDir}).`);
    const r = run('npm', ['uninstall', '-g', 'codex-companion']);
    log(r.ok ? 'npm uninstall -g codex-companion ok' : 'run: npm uninstall -g codex-companion');
  }

  log('uninstalled. The board keeps its last frame until it is unplugged.');
  return 0;
}

/* ------------------------------------------------------------------ *
 * cmd: status
 * ------------------------------------------------------------------ */

async function cmdStatus() {
  log(`package     ${PKG_ROOT}`);
  log(`install dir ${installDir()} ${fs.existsSync(installDir()) ? '(present)' : '(absent)'}`);

  if (process.platform === 'darwin') {
    const p = plistPath();
    log(`plist       ${p} ${fs.existsSync(p) ? '(present)' : '(absent)'}`);
    const st = run('launchctl', ['print', `gui/${uid()}/${LABEL}`]);
    if (st.ok) {
      const pid = /\bpid = (\d+)/.exec(st.stdout);
      const last = /last exit code = (\S+)/.exec(st.stdout);
      log(`service     loaded${pid ? `, pid ${pid[1]}` : ''}${last ? `, last exit ${last[1]}` : ''}`);
    } else {
      log('service     not loaded');
    }
    log(`log         ${logPath()} ${fs.existsSync(logPath()) ? '(present)' : '(absent)'}`);
  } else if (process.platform === 'linux') {
    const u = systemdUnitPath();
    log(`unit        ${u} ${fs.existsSync(u) ? '(present)' : '(absent)'}`);
    const st = run('systemctl', ['--user', 'is-active', 'codex-companion']);
    log(`service     ${st.stdout || 'unknown'}`);
  } else {
    log('service     autostart is manual on this platform');
  }

  const home = codexHome();
  const newest = await newestSessionFile(path.join(home, 'sessions'));
  log(`codex home  ${home}`);
  log(`session     ${newest ? newest.path : '(none found)'}`);
  return 0;
}

/* ------------------------------------------------------------------ *
 * cmd: doctor
 * ------------------------------------------------------------------ */

async function cmdDoctor(args) {
  const flags = args.flags || {};
  log(`node        ${process.version} (${process.execPath})`);
  const stable = stableNodeBin(process.execPath);
  if (stable !== process.execPath) {
    log(`            install would use the stable alias ${stable}`);
  } else if (isVolatileNodeBin(stable)) {
    log(`            WARNING: this path disappears on a node upgrade, and the`);
    log('            LaunchAgent points straight at it. Re-run install after one.');
  }
  log(`platform    ${process.platform} ${process.arch}`);

  let ports = [];
  let serialErr = null;
  try {
    const { SerialPort } = require('serialport');
    ports = await SerialPort.list();
  } catch (e) {
    serialErr = e;
  }

  if (serialErr) {
    log(`serialport  FAILED to load: ${serialErr.message}`);
    log('            try: npm rebuild @serialport/bindings-cpp');
  } else {
    log(`serialport  ok, ${ports.length} port(s):`);
    for (const p of ports) {
      const bits = [p.path];
      if (p.vendorId) bits.push(`vid=${p.vendorId}`);
      if (p.productId) bits.push(`pid=${p.productId}`);
      if (p.manufacturer) bits.push(`mfr=${p.manufacturer}`);
      log(`            - ${bits.join(' ')}`);
    }
    const m = matchPorts(ports, {
      port: typeof flags.port === 'string' ? flags.port : undefined
    });
    log(`device      ${m.chosen ? m.chosen.path : 'NOT FOUND'} - ${m.reason}`);
  }

  const home = flags['codex-home'] ? String(flags['codex-home']) : codexHome();
  const sessionsDir = path.join(home, 'sessions');
  log(`codex home  ${home} ${fs.existsSync(sessionsDir) ? '' : '(sessions/ missing)'}`);

  const newest = await newestSessionFile(sessionsDir);
  if (!newest) {
    log('session     none found. Run codex once, then re-run doctor.');
  } else {
    log(`session     ${newest.path}`);
    log(`            ${(newest.size / 1024).toFixed(1)} KB, modified ${new Date(newest.mtimeMs).toISOString()}`);
  }

  const watcher = new CodexWatcher({ codexHome: home });
  await watcher.rescan();
  await watcher.poll();
  const snap = watcher.getState();
  watcher.stop();

  log('state       ' + JSON.stringify({
    state: snap.state,
    heuristic: snap.heuristic,
    ctxFill: snap.ctxFill,
    contextWindow: snap.contextWindow,
    contextTokens: snap.contextTokens,
    totalTokens: snap.totalTokens,
    tps: snap.tps === null ? null : Math.round(snap.tps * 10) / 10,
    elapsedSec: snap.elapsedSec === null ? null : Math.round(snap.elapsedSec * 10) / 10,
    model: snap.model,
    sessionId: snap.sessionId,
    approvalPolicy: snap.approvalPolicy
  }));
  log('frame       ' + JSON.stringify(buildFrame(snap)));
  if (snap.approvalPolicy === 'never') {
    log('note        approval_policy is "never", so the waiting heuristic stays off.');
  }
  return 0;
}

/* ------------------------------------------------------------------ *
 * cmd: demo
 * ------------------------------------------------------------------ */

/** A scripted sequence that exercises every state, for testing a board. */
function demoScript() {
  return [
    { ms: 1500, state: { state: 'idle', model: 'gpt-5-codex' } },
    { ms: 4000, state: { state: 'busy', ctxFill: 0.08, elapsedSec: 3, tps: 12.4 } },
    { ms: 4000, state: { state: 'busy', ctxFill: 0.31, elapsedSec: 47, tps: 21.9 } },
    { ms: 6000, state: { state: 'waiting', ctxFill: 0.34, elapsedSec: 71, tps: 0 } },
    { ms: 3000, state: { state: 'busy', ctxFill: 0.52, elapsedSec: 118, tps: 18.2 } },
    { ms: 3000, state: { state: 'busy', ctxFill: 0.87, elapsedSec: 224, tps: 9.6 } },
    { ms: 3000, state: { state: 'done', ctxFill: 0.89, elapsedSec: 251 } },
    { ms: 3000, state: { state: 'idle', ctxFill: 0.89, elapsedSec: 251 } },
    { ms: 3000, state: { state: 'sleep' } }
  ];
}

async function cmdDemo(args) {
  const flags = args.flags || {};
  const loop = Boolean(flags.loop);
  const dry = Boolean(flags['dry-run']);
  const script = demoScript();

  let link = null;
  let everOpened = false;
  if (!dry) {
    link = new DeviceLink({ port: typeof flags.port === 'string' ? flags.port : undefined });
    link.on('open', (p) => {
      everOpened = true;
      log(`[device] open ${p}`);
    });
    link.on('hello', (l) => log(`[device] hello ${l === null ? '(timed out, proceeding)' : l}`));
    attachDeviceErrorLogging(link);
    // Bench-checking 14 units means a silent 30 seconds must never look like
    // success. DeviceLink emits 'scan' on every failed sweep; nothing else fires.
    let lastScanReason = null;
    link.on('scan', (r) => {
      if (r && r.chosen) return;
      const reason = (r && r.reason) || 'no port matched';
      if (reason === lastScanReason) return;
      lastScanReason = reason;
      log(`[device] no board found yet: ${reason}`);
    });
    link.start();
  }

  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  do {
    for (const step of script) {
      const frame = buildFrame(step.state);
      if (link) link.send(frame);
      else process.stdout.write(JSON.stringify(frame) + '\n');
      // Re-send inside the step so a slow-to-enumerate board still catches it.
      const until = Date.now() + step.ms;
      while (Date.now() < until) {
        await sleep(Math.min(250, until - Date.now()));
        if (link) link.send(frame);
      }
    }
  } while (loop);

  if (link) link.stop();
  if (link && !everOpened) {
    log('[device] NOT FOUND - no port matched vendorId 303a or an Espressif/LILYGO name');
    log('          check the cable (charge-only USB-C cables carry no data),');
    log('          then run: codex-companion doctor');
    return 1;
  }
  return 0;
}

/* ------------------------------------------------------------------ *
 * dispatch
 * ------------------------------------------------------------------ */

const USAGE = `codex-companion - desk companion for the Codex CLI

Usage:
  codex-companion                  install, then run in the foreground
  codex-companion run              stream live state to the device
  codex-companion install          install to ~/.codex-companion/app + autostart
  codex-companion uninstall        stop autostart and remove everything
  codex-companion status           is it installed, is it running
  codex-companion demo             scripted state sequence, no Codex needed
  codex-companion doctor           ports, device match, newest session, state

Options:
  --port <path>      use this serial port instead of auto-detecting
  --dry-run          print frames to stdout instead of opening a port
  --loop             (demo) repeat the sequence forever
  --global           (install) npm install -g this package instead of copying
  --codex-home <dir> override $CODEX_HOME
  --no-heuristic     never infer "waiting" from a stalled turn
  -h, --help         print this and exit, changing nothing
  --version          print the version and exit, changing nothing
`;

async function main(argv) {
  const args = parseArgs(argv);

  // These must be answered before the switch. parseArgs puts bare long flags in
  // `flags`, never in `_`, so `--help` used to fall through to `case 'default'`,
  // which installs a LaunchAgent and starts a background service.
  if (args.flags.help || args.flags.h) {
    process.stdout.write(USAGE);
    return 0;
  }
  if (args.flags.version) {
    log(require('../package.json').version);
    return 0;
  }

  const cmd = args._[0] || 'default';

  switch (cmd) {
    case 'run':
      return cmdRun(args);
    case 'install':
      return cmdInstall(args);
    case 'uninstall':
      return cmdUninstall(args);
    case 'status':
      return cmdStatus(args);
    case 'doctor':
      return cmdDoctor(args);
    case 'demo':
      return cmdDemo(args);
    case 'help':
    case '-h':
      process.stdout.write(USAGE);
      return 0;
    case 'default': {
      // This branch mutates ~/Library/LaunchAgents and starts a service, so it
      // only runs for a bare invocation, never as the fallback for a stray flag.
      const stray = Object.keys(args.flags).filter((f) => !DEFAULT_OK_FLAGS.has(f));
      if (stray.length) {
        log(`unknown option --${stray[0]}`);
        process.stdout.write(USAGE);
        return 1;
      }
      const code = await cmdInstall(args);
      if (code !== 0) return code;
      log('');
      log('installed. now streaming in the foreground (ctrl-c to stop;');
      log('the background service keeps running either way).');
      return cmdRun(args);
    }
    default:
      process.stdout.write(USAGE);
      return 1;
  }
}

module.exports = {
  main,
  parseArgs,
  BOOLEAN_FLAGS,
  DEFAULT_OK_FLAGS,
  xmlEscape,
  isVolatileNodeBin,
  stableNodeBin,
  renderPlist,
  renderSystemdUnit,
  demoScript,
  installDir,
  plistPath,
  systemdUnitPath,
  TEMPLATE_PATH,
  LABEL,
  USAGE
};
