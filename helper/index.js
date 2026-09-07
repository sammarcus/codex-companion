'use strict';

/**
 * Package entry point. The LaunchAgent plist runs `node <install>/index.js run`,
 * so this file has to be both the library surface and a runnable CLI.
 */

const frame = require('./src/frame');
const watcher = require('./src/codex-watcher');
const device = require('./src/device');
const install = require('./src/install');

module.exports = {
  ...frame,
  ...watcher,
  ...device,
  main: install.main
};

if (require.main === module) {
  install
    .main(process.argv.slice(2))
    .then((code) => {
      if (typeof code === 'number' && code !== 0) process.exit(code);
    })
    .catch((err) => {
      process.stderr.write(`codex-companion: ${err && err.stack ? err.stack : err}\n`);
      process.exit(1);
    });
}
