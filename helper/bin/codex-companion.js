#!/usr/bin/env node
'use strict';

const { main } = require('../src/install');

main(process.argv.slice(2))
  .then((code) => {
    if (typeof code === 'number' && code !== 0) process.exit(code);
  })
  .catch((err) => {
    process.stderr.write(`codex-companion: ${err && err.stack ? err.stack : err}\n`);
    process.exit(1);
  });
