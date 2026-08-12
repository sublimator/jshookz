#!/usr/bin/env node
"use strict";

const fs = require("node:fs");

function usage() {
  console.error("usage: node decode-fixtures.cjs <definitions.json> <items.json>");
}

if (process.argv.length !== 4) {
  usage();
  process.exit(64);
}

let codec;
try {
  codec = require("ripple-binary-codec");
} catch (error) {
  console.error(`ripple-binary-codec is not installed: ${error.message}`);
  process.exit(69);
}

const { decode, XrplDefinitions } = codec;
const definitionsJson = JSON.parse(fs.readFileSync(process.argv[2], "utf8"));
const items = JSON.parse(fs.readFileSync(process.argv[3], "utf8"));
const definitions = new XrplDefinitions(definitionsJson);

const rows = items.map((item) => {
  try {
    return {
      category: item.category,
      index: item.index,
      name: item.name,
      decoded: decode(item.hex, definitions),
    };
  } catch (error) {
    return {
      category: item.category,
      index: item.index,
      name: item.name,
      error: String(error && error.message ? error.message : error),
    };
  }
});

process.stdout.write(`${JSON.stringify(rows)}\n`);
