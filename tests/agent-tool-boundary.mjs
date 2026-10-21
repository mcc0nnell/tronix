// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';
import {createRestrictedToolHandler, allowedToolName} from '../src/agent-tool-boundary.mjs';

const checks = [];
function check(name, condition) {
  assert.ok(condition, name);
  checks.push(name);
}
const calls = [];
const stub = {
  async submit(...args) { calls.push(['submit', ...args]); return {ok: true}; },
  async result(id) { calls.push(['result', id]); return {ok: true, state: 'completed', value: 42}; },
  async ack(id) { calls.push(['ack', id]); return {ok: true}; }
};
const handler = createRestrictedToolHandler(stub);
const attacks = [
  null, 0, 'demo.constant', [], Object.create(null), {},
  {tool: 'exec'}, {tool: 'Runtime.evaluate'}, {tool: allowedToolName, command: 'id'},
  {tool: allowedToolName, url: 'http://127.0.0.1:1'},
  {tool: allowedToolName, agent: 'admin'},
  {tool: allowedToolName, deadlineMs: 999999},
  {tool: allowedToolName, operation: 'demo.overrun'},
  JSON.parse('{"tool":"tronix_demo_constant","__proto__":{"admin":true}}')
];
for (const attack of attacks) await assert.rejects(handler(attack), /DENIED/);
check('malformed, unknown, and parameter-smuggling requests rejected', attacks.length === 14);
check('every denied request rejected before IPC', calls.length === 0);
const a = await handler({tool: allowedToolName});
const b = await handler({tool: allowedToolName});
check('allowed request returns synthetic result', a.ok && b.ok && a.value === 42 && b.value === 42);
const submits = calls.filter(x => x[0] === 'submit');
check('static adapter chooses agent operation and deadline',
  submits.length === 2 && submits.every(x =>
    x[2] === 'tool-bridge-fixture' && x[3] === 'demo.constant' && x[4] === 2000));
check('adapter supplies unique opaque run IDs', submits[0][1] !== submits[1][1]);

const dir = await mkdtemp(join(tmpdir(), 'tronix-tool-boundary-'));
const root = resolve(import.meta.dirname, '..');
const socket = join(dir, 'tronix.sock');
const child = spawn(join(root, 'build/tronixd'),
  ['--socket', socket, '--scheduler', join(root, 'build/tronix_scheduler.zip'),
   '--adapter', join(root, 'build/tronix_demo.zip')],
  {cwd: root, stdio: ['ignore', 'pipe', 'pipe']});
let stdout = '', stderr = '';
child.stdout.on('data', x => stdout += x.toString());
child.stderr.on('data', x => stderr += x.toString());
try {
  for (let i = 0; i < 150; i++) {
    if (stdout.includes('TRONIX_READY')) break;
    if (child.exitCode !== null) throw Error('daemon failed: ' + stderr);
    await delay(20);
  }
  check('isolated native Tronix fixture ready', stdout.includes('TRONIX_READY'));
  const live = createRestrictedToolHandler(new TronixClient(socket));
  await assert.rejects(live({tool: allowedToolName, shell: 'bash'}), /DENIED/);
  const observed = await live({tool: allowedToolName});
  check('real Celix operation completes through bounded adapter',
    observed.ok === true && observed.state === 'completed' && observed.value === 42);
  const receipt = await new TronixClient(socket).receipt(observed.id);
  check('run receipt remains queryable after terminal acknowledgement',
    receipt.ok === true && receipt.receipt?.state === 'acked' && receipt.receipt?.value === 42);
  const nativeDenied = await new TronixClient(socket).submit(
    'negative-native', 'tool-bridge-fixture', 'Runtime.evaluate', 2000);
  check('underlying Celix adapter also refuses unrecognized operations',
    nativeDenied.ok === false && nativeDenied.error === 'DENIED');
  console.log(JSON.stringify({schema: 'tronix.tool-boundary.test.v1', outcome: 'PASS',
    checks, negativeCases: attacks.length + 1}, null, 2));
} finally {
  child.kill('SIGTERM');
  await Promise.race([new Promise(resolve => child.once('exit', resolve)), delay(3000)]);
  if (child.exitCode === null) child.kill('SIGKILL');
  await rm(dir, {recursive: true, force: true});
}
