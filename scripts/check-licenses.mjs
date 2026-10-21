#!/usr/bin/env node
// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0

// Local, dependency-free licensing guard for first-party Tronix source.
// Third-party checkouts and executables must retain their own licenses.
import {readFileSync, readdirSync, statSync} from 'node:fs';
import {join, extname} from 'node:path';
import {fileURLToPath} from 'node:url';

const root = fileURLToPath(new URL('../', import.meta.url));
const failures = [];
const read = path => readFileSync(join(root, path), 'utf8');
const requireText = (path, expected) => {
  try {
    const text = read(path);
    if (!text.includes(expected)) failures.push(`${path}: missing expected marker ${expected}`);
  } catch (error) {
    failures.push(`${path}: ${error.code ?? error.message}`);
  }
};
requireText('LICENSE', 'Apache License');
requireText('LICENSE', 'END OF TERMS AND CONDITIONS');
requireText('NOTICE', 'Copyright 2026 Robert McConnell');
requireText('THIRD_PARTY_LICENSES.md', 'Lightpanda');
requireText('CMakeLists.txt', 'SPDX-License-Identifier: Apache-2.0');

for (const path of ['package.json', 'package-lock.json']) {
  try {
    const data = JSON.parse(read(path));
    const id = path === 'package.json' ? data.license : data.packages?.['']?.license;
    if (id !== 'Apache-2.0') failures.push(`${path}: license must be Apache-2.0`);
  } catch (error) {
    failures.push(`${path}: ${error.message}`);
  }
}

const extensions = new Set(['.c', '.cc', '.h', '.hpp', '.mjs', '.py']);
let checked = 0;
function inspect(dir) {
  for (const entry of readdirSync(join(root, dir), {withFileTypes: true})) {
    const rel = join(dir, entry.name);
    if (entry.isDirectory()) { inspect(rel); continue; }
    if (!extensions.has(extname(rel))) continue;
    if (!entry.isFile()) {
      failures.push(`${rel}: source entry must be a regular file`);
      continue;
    }
    const content = read(rel);
    const matches = content.match(/SPDX-License-Identifier: Apache-2\.0/g) || [];
    if (matches.length !== 1 || !content.split('\n').slice(0, 12).some(line =>
      /SPDX-License-Identifier: Apache-2\.0/.test(line))) {
      failures.push(`${rel}: exactly one Apache-2.0 SPDX header required near file start`);
    }
    checked++;
  }
}
for (const dir of ['include', 'src', 'tests', 'scripts']) inspect(dir);
if (checked < 32) failures.push(`expected at least 32 first-party source files; found ${checked}`);
if (failures.length) {
  for (const error of failures) console.error('LICENSE CHECK:', error);
  process.exitCode = 1;
} else {
  console.log(`License check passed: ${checked} first-party source files; root license, notice and npm metadata verified`);
}
