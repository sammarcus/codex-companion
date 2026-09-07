'use strict';

const test = require('node:test');
const assert = require('node:assert');
const { EventEmitter } = require('node:events');

const {
  DeviceLink,
  matchPorts,
  scorePort,
  ESP_VENDOR_ID,
  ESP_S3_PRODUCT_ID,
  HELLO_RE
} = require('../src/device');

/**
 * Real SerialPort.list() output from this machine (docs/host-tooling-recon.md).
 * Two genuine USB-serial devices, neither of them an ESP32: a useful negative.
 */
const REAL_PORTS = [
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
];

/**
 * Expected shape for a T-Display-S3 in native USB CDC mode. UNCONFIRMED against
 * live hardware: derived from the board definition's hwids [["0X303A","0x1001"]]
 * plus the lowercase-no-0x formatting observed on the real ports above.
 */
const TDISPLAY = {
  path: '/dev/tty.usbmodem101',
  manufacturer: 'Espressif',
  serialNumber: '34:85:18:91:2A:B0',
  vendorId: '303a',
  productId: '1001'
};

test('vendor and product ids are the lowercase-no-prefix form list() reports', () => {
  assert.strictEqual(ESP_VENDOR_ID, '303a');
  assert.strictEqual(ESP_S3_PRODUCT_ID, '1001');
});

test('no ESP32 attached: nothing is chosen, and nothing is guessed', () => {
  const m = matchPorts(REAL_PORTS, {});
  assert.strictEqual(m.chosen, null);
  assert.deepStrictEqual(m.candidates, []);
  assert.match(m.reason, /no port matched/);
});

test('the board is picked out of a crowded port list', () => {
  const m = matchPorts(REAL_PORTS.concat([TDISPLAY]), {});
  assert.strictEqual(m.chosen.path, '/dev/tty.usbmodem101');
  assert.strictEqual(m.candidates.length, 1);
  assert.match(m.reason, /score 100/);
});

test('scorePort ranking table', () => {
  const cases = [
    { name: 'exact vid+pid', port: TDISPLAY, want: 100 },
    {
      name: 'Espressif vid with a different pid (custom USB_PID build)',
      port: { path: '/dev/ttyACM0', vendorId: '303a', productId: '4001' },
      want: 80
    },
    {
      name: 'uppercase vid still matches',
      port: { path: '/dev/ttyACM0', vendorId: '303A', productId: '1001' },
      want: 100
    },
    {
      name: 'manufacturer string fallback for a clone board',
      port: { path: '/dev/ttyUSB0', vendorId: '1a86', productId: '7523', manufacturer: 'LILYGO' },
      want: 40
    },
    {
      name: 'pnpId fallback on Windows',
      port: { path: 'COM7', pnpId: 'USB\\VID_303A&PID_1001\\34851891' },
      want: 40
    },
    { name: 'unrelated USB serial device', port: REAL_PORTS[3], want: 0 },
    { name: 'bluetooth pseudo-port', port: REAL_PORTS[1], want: 0 },
    {
      name: 'a hex serial number that happens to contain a hint is not a match',
      port: { path: '/dev/ttyUSB9', vendorId: '0403', productId: '6001', serialNumber: 'ESP32FAKE' },
      want: 0
    },
    { name: 'a port with no path is never a candidate', port: { vendorId: '303a' }, want: 0 },
    { name: 'null port', port: null, want: 0 }
  ];
  for (const c of cases) {
    assert.strictEqual(scorePort(c.port, {}), c.want, c.name);
  }
});

test('--port overrides every heuristic, and only matches exactly', () => {
  const all = REAL_PORTS.concat([TDISPLAY]);
  const m = matchPorts(all, { port: '/dev/tty.muffins' });
  assert.strictEqual(m.chosen.path, '/dev/tty.muffins');
  assert.strictEqual(m.candidates.length, 1, 'the override excludes the auto-detected board');

  const miss = matchPorts(all, { port: '/dev/tty.nonexistent' });
  assert.strictEqual(miss.chosen, null);
  assert.match(miss.reason, /no port matching --port/);
});

test('a custom vendorId/productId can be supplied', () => {
  const odd = { path: '/dev/ttyACM3', vendorId: '1a86', productId: '55d4' };
  const m = matchPorts([odd], { vendorId: '1a86', productId: '55d4' });
  assert.strictEqual(m.chosen.path, '/dev/ttyACM3');
});

test('multiple boards: one is picked deterministically and the tie is reported', () => {
  const a = Object.assign({}, TDISPLAY, { path: '/dev/tty.usbmodem2101' });
  const b = Object.assign({}, TDISPLAY, { path: '/dev/tty.usbmodem1101' });
  const m = matchPorts([a, b], {});
  assert.strictEqual(m.candidates.length, 2);
  assert.strictEqual(m.chosen.path, '/dev/tty.usbmodem1101', 'sorted by path, stable across runs');
  assert.match(m.reason, /2 ports tied/);
  assert.match(m.reason, /--port to override/);

  // Same two ports in the other order must give the same answer.
  assert.strictEqual(matchPorts([b, a], {}).chosen.path, '/dev/tty.usbmodem1101');
});

test('an exact vid+pid match outranks a name-only match', () => {
  const nameOnly = { path: '/dev/ttyUSB0', manufacturer: 'LILYGO', vendorId: '1a86' };
  const m = matchPorts([nameOnly, TDISPLAY], {});
  assert.strictEqual(m.chosen.path, TDISPLAY.path);
  assert.strictEqual(m.candidates.length, 2, 'the weaker match is still offered as a candidate');
  assert.match(m.reason, /score 100/);
});

test('matchPorts survives junk input', () => {
  for (const input of [null, undefined, [], [null], [{}], ['nope']]) {
    const m = matchPorts(input, {});
    assert.strictEqual(m.chosen, null);
  }
});

test('the hello handshake pattern matches the documented banner', () => {
  assert.ok(HELLO_RE.test('hello tdisplay-s3 v1'));
  assert.ok(HELLO_RE.test('HELLO TDISPLAY-S3 v2'));
  assert.ok(HELLO_RE.test('  hello tdisplay-s3 v1  '.trim()));
  assert.ok(!HELLO_RE.test('ok'));
  assert.ok(!HELLO_RE.test('hello world'));
});

/* --------------------------------------------------------------------- *
 * DeviceLink against a fake serialport module. No real port is ever opened.
 * --------------------------------------------------------------------- */

function fakeSerial(ports) {
  const opened = [];

  class FakePort extends EventEmitter {
    constructor(opts, cb) {
      super();
      this.path = opts.path;
      this.baudRate = opts.baudRate;
      this.written = [];
      this.isOpen = true;
      opened.push(this);
      // Open asynchronously, like the real thing.
      setImmediate(() => cb && cb(null));
    }

    pipe(parser) {
      this.parser = parser;
      return parser;
    }

    write(data) {
      this.written.push(data);
      return true;
    }

    close(cb) {
      this.isOpen = false;
      if (cb) cb(null);
      this.emit('close');
    }

    /** Simulate a physical unplug: 'close' with err.disconnected === true. */
    unplug() {
      this.isOpen = false;
      const err = new Error('Port is not open');
      err.disconnected = true;
      this.emit('close', err);
    }

    /** Simulate a line arriving from the board. */
    feed(line) {
      if (this.parser) this.parser.emit('data', line);
    }
  }

  class FakeParser extends EventEmitter {}

  return {
    opened,
    mod: {
      SerialPort: Object.assign(FakePort, { list: async () => ports }),
      ReadlineParser: FakeParser
    }
  };
}

const tick = () => new Promise((r) => setImmediate(r));

test('DeviceLink opens the matched port at 115200 and waits for hello', async () => {
  const fake = fakeSerial(REAL_PORTS.concat([TDISPLAY]));
  const clock = { t: 0 };
  const link = new DeviceLink({
    serial: fake.mod,
    now: () => clock.t,
    helloTimeoutMs: 3000
  });

  const events = [];
  link.on('open', (p) => events.push(['open', p]));
  link.on('hello', (l) => events.push(['hello', l]));

  await link.connect();
  await tick();

  assert.strictEqual(fake.opened.length, 1);
  assert.strictEqual(fake.opened[0].path, '/dev/tty.usbmodem101');
  assert.strictEqual(fake.opened[0].baudRate, 115200);
  assert.strictEqual(link.connected, true);
  assert.strictEqual(link.helloSeen, false);

  fake.opened[0].feed('hello tdisplay-s3 v1');
  assert.strictEqual(link.helloSeen, true);
  assert.deepStrictEqual(events, [
    ['open', '/dev/tty.usbmodem101'],
    ['hello', 'hello tdisplay-s3 v1']
  ]);
  link.stop();
});

test('DeviceLink never opens anything when no board is present', async () => {
  const fake = fakeSerial(REAL_PORTS);
  const link = new DeviceLink({ serial: fake.mod, scanMs: 10 });
  const scans = [];
  link.on('scan', (r) => scans.push(r));
  await link.connect();
  await tick();
  assert.strictEqual(fake.opened.length, 0);
  assert.strictEqual(link.connected, false);
  assert.strictEqual(scans[0].chosen, null);
  link.stop();
});

test('frames are throttled to 250 ms, deduplicated, and heartbeated every 2 s', async () => {
  const fake = fakeSerial([TDISPLAY]);
  const clock = { t: 1000 };
  const link = new DeviceLink({ serial: fake.mod, now: () => clock.t });
  await link.connect();
  await tick();
  const port = fake.opened[0];
  port.feed('hello tdisplay-s3 v1');

  const busy = { state: 'busy', ctxFill: 0.62, elapsedSec: 754, tps: 17.34 };

  link.update(busy);
  assert.strictEqual(port.written.length, 1, 'first frame goes out immediately');
  assert.strictEqual(
    port.written[0],
    '{"state":"busy","ring":0.62,"center":"62%","label":"CTX","sub":"12:34 elapsed","tps":17.3}\n'
  );

  // Same frame, 100 ms later: below the 250 ms floor, nothing sent.
  clock.t += 100;
  link.update(busy);
  assert.strictEqual(port.written.length, 1);

  // Same frame, 300 ms in: past the floor but unchanged and not yet a heartbeat.
  clock.t += 200;
  link.update(busy);
  assert.strictEqual(port.written.length, 1);

  // A changed frame past the floor goes out.
  clock.t += 300;
  link.update({ state: 'waiting', ctxFill: 0.62, elapsedSec: 780, tps: 0 });
  assert.strictEqual(port.written.length, 2);
  assert.match(port.written[1], /"state":"waiting"/);
  assert.ok(!/"tps"/.test(port.written[1]), 'a zero rate is omitted, not sent as 0');

  // Unchanged, but 2 s since the last write: heartbeat.
  clock.t += 2000;
  link.update({ state: 'waiting', ctxFill: 0.62, elapsedSec: 780, tps: 0 });
  assert.strictEqual(port.written.length, 3, 'heartbeat keeps the device from dimming');
  assert.strictEqual(port.written[2], port.written[1]);

  for (const line of port.written) {
    assert.ok(line.endsWith('\n'), 'every frame is newline terminated');
    assert.doesNotThrow(() => JSON.parse(line), 'every frame is one JSON object');
  }
  link.stop();
});

test('nothing is written while the link is down', () => {
  const fake = fakeSerial([TDISPLAY]);
  const link = new DeviceLink({ serial: fake.mod });
  assert.strictEqual(link.connected, false);
  link.update({ state: 'busy', ctxFill: 0.5 });
  assert.strictEqual(fake.opened.length, 0);
  link.stop();
});

test('an unplug is detected as close+disconnected and schedules a reconnect', async () => {
  const fake = fakeSerial([TDISPLAY]);
  const clock = { t: 0 };
  const link = new DeviceLink({
    serial: fake.mod,
    now: () => clock.t,
    reconnectMinMs: 5,
    reconnectMaxMs: 20
  });
  const closes = [];
  link.on('close', (e) => closes.push(e));

  await link.connect();
  await tick();
  assert.strictEqual(link.connected, true);

  fake.opened[0].unplug();
  assert.strictEqual(link.connected, false);
  assert.strictEqual(closes.length, 1);
  assert.strictEqual(closes[0].disconnected, true, 'unplug arrives as close, not disconnect');

  // The backoff timer re-opens the port once it is back.
  await new Promise((r) => setTimeout(r, 60));
  assert.ok(fake.opened.length >= 2, 'reconnected after the board came back');
  link.stop();
});

test('reconnect backoff doubles up to the cap', async () => {
  const fake = fakeSerial([TDISPLAY]);
  const link = new DeviceLink({ serial: fake.mod, reconnectMinMs: 100, reconnectMaxMs: 400 });
  const delays = [];
  link._scheduleLoop = (ms) => delays.push(ms);
  link._stopped = false;
  link._backoff();
  link._backoff();
  link._backoff();
  link._backoff();
  assert.deepStrictEqual(delays, [100, 200, 400, 400]);
  link.stop();
});

test('a fresh connection resends the full frame even if it is unchanged', async () => {
  const fake = fakeSerial([TDISPLAY]);
  const clock = { t: 0 };
  const link = new DeviceLink({
    serial: fake.mod,
    now: () => clock.t,
    reconnectMinMs: 5,
    reconnectMaxMs: 20
  });
  await link.connect();
  await tick();
  const first = fake.opened[0];
  first.feed('hello tdisplay-s3 v1');
  link.update({ state: 'busy', ctxFill: 0.5, elapsedSec: 10 });
  assert.strictEqual(first.written.length, 1);

  first.unplug();
  await new Promise((r) => setTimeout(r, 80));
  const second = fake.opened[fake.opened.length - 1];
  assert.notStrictEqual(second, first);
  second.feed('hello tdisplay-s3 v1');
  clock.t += 10;
  link.update({ state: 'busy', ctxFill: 0.5, elapsedSec: 10 });
  assert.strictEqual(second.written.length, 1, 'the new link starts from a known frame');
  link.stop();
});
