'use strict';

/**
 * device.js - find the T-Display-S3, open it at 115200, stream frames.
 *
 * Grounded in docs/host-tooling-recon.md:
 *  - SerialPort.list() reports vendorId/productId as lowercase hex, no 0x
 *    prefix. Non-USB ports omit those fields entirely (undefined).
 *  - vendor/T-Display-S3/boards/lilygo-t-display-s3.json declares hwids
 *    [["0X303A","0x1001"]]: Espressif VID 303a, native-USB PID 1001.
 *    UNCONFIRMED against live hardware - no board has ever been plugged in.
 *  - There is no 'disconnect' event. Unplug surfaces as 'close' with an error
 *    whose .disconnected === true. There is no built-in reconnect.
 *  - ReadlineParser is re-exported from the top-level `serialport` package.
 */

const { EventEmitter } = require('node:events');
const { buildFrame, framesEqual } = require('./frame');

/** Espressif Systems USB vendor id, as SerialPort.list() formats it. */
const ESP_VENDOR_ID = '303a';
/** PID declared by the LILYGO board definition for native USB CDC. */
const ESP_S3_PRODUCT_ID = '1001';

/**
 * Name fallback for boards that do not report Espressif's VID (a clone, a
 * custom -DUSB_PID, or a USB-UART bridge chip in front of the ESP32). Matched
 * against manufacturer and pnpId only - never against serialNumber, which is
 * hex and would collide on substrings.
 */
const NAME_HINT = /espressif|esp32|esp-?s3|lilygo|t-?display|usb\s*jtag/i;

const DEFAULTS = {
  baudRate: 115200,
  /** how long to wait for the device's hello line before proceeding anyway */
  helloTimeoutMs: 3000,
  /** minimum spacing between frames */
  frameMinMs: 250,
  /** send an unchanged frame at least this often, as a heartbeat */
  heartbeatMs: 2000,
  reconnectMinMs: 500,
  reconnectMaxMs: 10000,
  /** how often to re-poll SerialPort.list() while disconnected */
  scanMs: 1500
};

const HELLO_RE = /hello\s+tdisplay-s3/i;

/**
 * Score one port as a T-Display-S3 candidate.
 * Higher is better; 0 means "not a candidate".
 */
function scorePort(p, opts) {
  const o = opts || {};
  if (!p || typeof p.path !== 'string' || !p.path) return 0;

  if (o.port) {
    return p.path === o.port ? 1000 : 0;
  }

  const vid = (p.vendorId || '').toLowerCase();
  const pid = (p.productId || '').toLowerCase();
  const wantVid = (o.vendorId || ESP_VENDOR_ID).toLowerCase();
  const wantPid = (o.productId || ESP_S3_PRODUCT_ID).toLowerCase();

  if (vid === wantVid && pid === wantPid) return 100;
  if (vid === wantVid) return 80;

  const text = `${p.manufacturer || ''} ${p.pnpId || ''}`;
  if (NAME_HINT.test(text)) return 40;
  if (/303a/i.test(p.pnpId || '')) return 40;

  return 0;
}

/**
 * Rank a SerialPort.list() result.
 * @returns {{candidates:Array,chosen:object|null,reason:string}}
 *
 * Deliberately does not guess when the evidence is thin: with zero scoring
 * ports the caller is told to ask the user for --port rather than opening
 * something random.
 */
function matchPorts(ports, opts) {
  const list = Array.isArray(ports) ? ports : [];
  const scored = list
    .map((p) => ({ port: p, score: scorePort(p, opts) }))
    .filter((x) => x.score > 0)
    .sort((a, b) => b.score - a.score || String(a.port.path).localeCompare(String(b.port.path)));

  const candidates = scored.map((x) => x.port);

  if (candidates.length === 0) {
    return {
      candidates,
      chosen: null,
      reason:
        opts && opts.port
          ? `no port matching --port ${opts.port}`
          : 'no port matched vendorId 303a or an Espressif/LILYGO name'
    };
  }

  const best = scored[0];
  const tied = scored.filter((x) => x.score === best.score);
  if (tied.length > 1) {
    return {
      candidates,
      chosen: best.port,
      reason: `${tied.length} ports tied at score ${best.score}; picked ${best.port.path} (use --port to override)`
    };
  }

  return { candidates, chosen: best.port, reason: `matched ${best.port.path} (score ${best.score})` };
}

/** Lazily require serialport so the pure logic above is testable without it. */
function loadSerialPort() {
  // eslint-disable-next-line global-require
  return require('serialport');
}

/**
 * DeviceLink - keeps one T-Display-S3 connected and fed with frames.
 *
 * Events: 'open'(path), 'hello'(line), 'line'(line), 'frame'(obj),
 *         'close'(err|undefined), 'error'(err), 'scan'(result)
 */
class DeviceLink extends EventEmitter {
  constructor(opts) {
    super();
    this.opts = Object.assign({}, DEFAULTS, opts || {});
    this.now = typeof this.opts.now === 'function' ? this.opts.now : () => Date.now();
    this.serial = this.opts.serial || null; // injectable for tests
    this.port = null;
    this.parser = null;
    this.connected = false;
    this.helloSeen = false;
    this.chosenPath = null;

    this._pending = null;
    this._lastFrame = null;
    // -Infinity, not 0: the first frame on any link must go out immediately,
    // whatever the clock happens to read.
    this._lastSentAt = -Infinity;
    this._sendTimer = null;
    this._reconnectDelay = this.opts.reconnectMinMs;
    this._stopped = true;
    this._helloTimer = null;
    this._loopTimer = null;
  }

  _serial() {
    if (!this.serial) this.serial = loadSerialPort();
    return this.serial;
  }

  async listPorts() {
    const { SerialPort } = this._serial();
    return SerialPort.list();
  }

  /** Scan and return the match result without connecting. */
  async findPort() {
    const ports = await this.listPorts();
    return matchPorts(ports, this.opts);
  }

  start() {
    this._stopped = false;
    this._loop();
    this._sendTimer = setInterval(() => this._maybeSend(), this.opts.frameMinMs);
    if (this._sendTimer.unref) this._sendTimer.unref();
    return this;
  }

  stop() {
    this._stopped = true;
    if (this._sendTimer) clearInterval(this._sendTimer);
    if (this._loopTimer) clearTimeout(this._loopTimer);
    if (this._helloTimer) clearTimeout(this._helloTimer);
    this._sendTimer = null;
    this._loopTimer = null;
    this._helloTimer = null;
    this._closePort();
  }

  /**
   * Scan once and open the board if it is there. Resolves whether or not a
   * connection was made; `connected` says which. `start()` is the same thing
   * plus the retry loop and the frame pump.
   */
  async connect() {
    this._stopped = false;
    await this._loop();
    return this.connected;
  }

  _scheduleLoop(ms) {
    if (this._stopped) return;
    if (this._loopTimer) clearTimeout(this._loopTimer);
    this._loopTimer = setTimeout(() => this._loop(), ms);
    if (this._loopTimer.unref) this._loopTimer.unref();
  }

  async _loop() {
    if (this._stopped || this.connected) return;
    let result;
    try {
      result = await this.findPort();
    } catch (e) {
      this.emit('error', e);
      this._scheduleLoop(this.opts.scanMs);
      return;
    }
    this.emit('scan', result);
    if (!result.chosen) {
      this._scheduleLoop(this.opts.scanMs);
      return;
    }
    try {
      await this._open(result.chosen.path);
    } catch (e) {
      this.emit('error', e);
      this._backoff();
    }
  }

  _backoff() {
    const d = this._reconnectDelay;
    this._reconnectDelay = Math.min(this._reconnectDelay * 2, this.opts.reconnectMaxMs);
    this._scheduleLoop(d);
  }

  _open(path) {
    const { SerialPort, ReadlineParser } = this._serial();
    return new Promise((resolve, reject) => {
      let settled = false;
      const port = new SerialPort({ path, baudRate: this.opts.baudRate }, (err) => {
        if (err) {
          if (!settled) {
            settled = true;
            reject(err);
          }
          return;
        }
        if (settled) return;
        settled = true;
        this.port = port;
        this.chosenPath = path;
        this.connected = true;
        this.helloSeen = false;
        this._reconnectDelay = this.opts.reconnectMinMs;
        // A fresh link knows nothing, so neither the dedupe nor the throttle
        // may suppress the first frame after a reconnect.
        this._lastFrame = null;
        this._lastSentAt = -Infinity;
        this.emit('open', path);
        // Wait up to helloTimeoutMs for "hello tdisplay-s3 v1", then proceed
        // regardless: a board that missed its boot banner is still usable.
        this._helloTimer = setTimeout(() => {
          this._helloTimer = null;
          if (!this.helloSeen) this.emit('hello', null);
          this._maybeSend(true);
        }, this.opts.helloTimeoutMs);
        if (this._helloTimer.unref) this._helloTimer.unref();
        resolve(port);
      });

      this.parser = port.pipe(new ReadlineParser({ delimiter: '\n' }));
      this.parser.on('data', (line) => this._onLine(String(line).trim()));

      port.on('error', (err) => {
        this.emit('error', err);
      });
      // Unplug arrives as 'close' with err.disconnected === true.
      port.on('close', (err) => {
        const wasConnected = this.connected;
        this.connected = false;
        this.port = null;
        this.parser = null;
        if (this._helloTimer) {
          clearTimeout(this._helloTimer);
          this._helloTimer = null;
        }
        if (wasConnected) this.emit('close', err);
        if (!settled) {
          settled = true;
          reject(err || new Error('port closed before open'));
          return;
        }
        this._backoff();
      });
    });
  }

  _onLine(line) {
    if (!line) return;
    this.emit('line', line);
    if (!this.helloSeen && HELLO_RE.test(line)) {
      this.helloSeen = true;
      if (this._helloTimer) {
        clearTimeout(this._helloTimer);
        this._helloTimer = null;
      }
      this.emit('hello', line);
      this._maybeSend(true);
    }
  }

  _closePort() {
    if (this.port) {
      try {
        this.port.close(() => {});
      } catch {
        /* already gone */
      }
    }
    this.port = null;
    this.parser = null;
    this.connected = false;
  }

  /** Queue a state snapshot; the send loop turns it into a frame. */
  update(state, frameOpts) {
    this._pending = buildFrame(state, frameOpts);
    this._maybeSend();
    return this._pending;
  }

  /** Send a frame object straight through (used by `demo`). */
  send(frame) {
    this._pending = frame;
    this._maybeSend();
    return frame;
  }

  _maybeSend(force) {
    if (!this.connected || !this.port || !this._pending) return false;
    const now = this.now();
    const since = now - this._lastSentAt;
    if (!force && since < this.opts.frameMinMs) return false;

    const changed = !framesEqual(this._lastFrame, this._pending);
    const heartbeatDue = since >= this.opts.heartbeatMs;
    if (!force && !changed && !heartbeatDue) return false;

    const line = JSON.stringify(this._pending) + '\n';
    try {
      this.port.write(line);
    } catch (e) {
      this.emit('error', e);
      return false;
    }
    this._lastFrame = this._pending;
    this._lastSentAt = now;
    this.emit('frame', this._pending);
    return true;
  }
}

module.exports = {
  DeviceLink,
  matchPorts,
  scorePort,
  ESP_VENDOR_ID,
  ESP_S3_PRODUCT_ID,
  HELLO_RE,
  DEFAULTS
};
