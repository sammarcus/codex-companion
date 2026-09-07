'use strict';

/**
 * Port discovery without the serialport package: ioreg text in, a decision out.
 * The ioreg sample below is trimmed from a real `ioreg -r -c IOUSBHostDevice -l`
 * on the machine this was developed on, with a real T-Display-S3 attached.
 */

const test = require('node:test');
const assert = require('node:assert');

const { parseIoreg, choosePort, ESP_VENDOR_ID, ESP_PRODUCT_ID } = require('../codex-companion');

const IOREG = `
+-o USB JTAG/serial debug unit@00100000  <class IOUSBHostDevice, id 0x100211fd2, registered>
  |   "USB Product Name" = "USB JTAG_serial debug unit"
  |   "idVendor" = 12346
  |   "idProduct" = 4097
  +-o IOUSBHostInterface@1  <class IOUSBHostInterface, id 0x10021206a, registered>
  | |   "idProduct" = 4097
  | |   "idVendor" = 12346
  |         "IOCalloutDevice" = "/dev/cu.usbmodem101"
  |         "IODialinDevice" = "/dev/tty.usbmodem101"
+-o Some Other Widget@02100000  <class IOUSBHostDevice, id 0x100000cb1, registered>
  |   "idVendor" = 1234
  |   "idProduct" = 5678
  |         "IOCalloutDevice" = "/dev/cu.usbmodem0054452"
`;

test('parseIoreg pairs a callout device with the USB ids above it', () => {
  const found = parseIoreg(IOREG);
  assert.deepStrictEqual(found, [
    { path: '/dev/cu.usbmodem101', vendorId: 12346, productId: 4097 },
    { path: '/dev/cu.usbmodem0054452', vendorId: 1234, productId: 5678 }
  ]);
  assert.strictEqual(ESP_VENDOR_ID, 12346);
  assert.strictEqual(ESP_PRODUCT_ID, 4097);
});

test('parseIoreg survives empty and garbage input', () => {
  assert.deepStrictEqual(parseIoreg(''), []);
  assert.deepStrictEqual(parseIoreg(null), []);
  assert.deepStrictEqual(parseIoreg('not ioreg output at all'), []);
});

test('the Espressif board is chosen even when other serial ports are present', () => {
  const pick = choosePort({
    ioreg: IOREG,
    ports: ['/dev/cu.usbmodem0054452', '/dev/cu.usbmodem101']
  });
  assert.strictEqual(pick.chosen, '/dev/cu.usbmodem101');
  assert.match(pick.reason, /303a:1001/);
});

test('two matching boards refuse to be guessed at and ask for --port', () => {
  const two = IOREG.replace('"idVendor" = 1234\n', '"idVendor" = 12346\n').replace(
    '"idProduct" = 5678',
    '"idProduct" = 4097'
  );
  const pick = choosePort({ ioreg: two, ports: ['/dev/cu.usbmodem0054452', '/dev/cu.usbmodem101'] });
  assert.strictEqual(pick.chosen, null);
  assert.strictEqual(pick.matched.length, 2);
  assert.match(pick.reason, /--port/);
});

test('no matching board says so, and says what it did see', () => {
  const pick = choosePort({ ioreg: '', ports: ['/dev/cu.usbmodem0054452'] });
  assert.strictEqual(pick.chosen, null);
  assert.match(pick.reason, /cu\.usbmodem0054452/);
  assert.match(pick.reason, /--port/);
});

test('no ports at all blames the cable, which is the usual cause', () => {
  const pick = choosePort({ ioreg: '', ports: [] });
  assert.strictEqual(pick.chosen, null);
  assert.match(pick.reason, /charge-only/);
});

test('an explicit --port wins and is honest that it did not verify identity', () => {
  const pick = choosePort({ ioreg: IOREG, ports: [], port: '/dev/cu.whatever' });
  assert.strictEqual(pick.chosen, '/dev/cu.whatever');
  assert.match(pick.reason, /you asked for this one/);
});

test('a board reported by ioreg but with no tty node is not chosen', () => {
  // Stale ioreg entries outlive an unplug for a moment; the /dev listing is
  // the ground truth for whether a port exists right now.
  const pick = choosePort({ ioreg: IOREG, ports: ['/dev/cu.usbmodem0054452'] });
  assert.strictEqual(pick.chosen, null);
});
