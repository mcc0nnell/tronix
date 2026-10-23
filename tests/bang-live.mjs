// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Opt-in end-to-end Celix fixture: signed synthetic captions, NEVER an RF trial.
import assert from 'node:assert/strict';
import {existsSync} from 'node:fs';
import {mkdtemp,chmod,rm} from 'node:fs/promises';
import {spawn,spawnSync} from 'node:child_process';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';

const root = resolve(import.meta.dirname, '..');
const bin = join(root, 'build/tronix_bang_fixture_test');
const bundle = join(root, 'build/tronix_bang.zip');
for (const path of [bin, bundle, join(root, 'build/tronixd'),
  join(root, 'build/tronix_scheduler.zip')]) {
  if (!existsSync(path)) throw new Error('Build tronix_bang_fixture_test, tronix_bang, tronixd and tronix_scheduler_bundle first: missing ' + path);
}
const local = spawnSync(bin, [], {cwd:root,encoding:'utf8',timeout:5000});
assert.equal(local.status, 0, 'native fixture failed: ' + local.stdout + local.stderr);

const dir = await mkdtemp(join(tmpdir(), 'tronix-bang-'));
await chmod(dir, 0o700);
const socket = join(dir, 'runtime.sock');
const child = spawn(join(root,'build/tronixd'), [
  '--socket',socket,
  '--scheduler',join(root,'build/tronix_scheduler.zip'),
  '--adapter',bundle
], {
  cwd:root, env:{...process.env, TRONIX_BANG_ENABLE_FIXTURE:'1'},
  stdio:['ignore','pipe','pipe']
});
let stdout='', stderr='';
child.stdout.on('data', c => {stdout=(stdout+c.toString()).slice(-8192);});
child.stderr.on('data', c => {stderr=(stderr+c.toString()).slice(-8192);});
const client = new TronixClient(socket);
const checks = [];
function check(label, actual) { assert.ok(actual, label); checks.push(label); }
async function ready() {
  for (let i=0; i<200; i++) {
    if (stdout.includes('TRONIX_READY')) return;
    if (child.exitCode !== null) throw Error('runtime exited: '+child.exitCode+' '+stderr);
    await delay(25);
  }
  throw Error('runtime readiness timeout: '+stderr);
}
async function terminal(id) {
  for (let i=0; i<100; i++) {
    const response=await client.result(id);
    if (['completed','failed','expired','overrun'].includes(response.state)) return response;
    await delay(20);
  }
  throw Error('job not terminal: '+id);
}
try {
  await ready();
  check('native Celix runtime started', (await client.ping()).ok);
  const caps=await client.capabilities();
  check('only two synthetic 6lowBang operations', caps.ok &&
    caps.operations.length===2 &&
    caps.operations.includes('bang.synthetic_delivery') &&
    caps.operations.includes('bang.synthetic_gap'));
  check('arbitrary Thread or speech actions denied',
    (await client.submit('no-raw','fixture','bang.raw_cli',4000)).error==='DENIED');
  for (const [id, op, expected] of [
    ['bang-captions','bang.synthetic_delivery',2],
    ['bang-gap','bang.synthetic_gap',1]
  ]) {
    check(op+' admitted', (await client.submit(id,'fixture',op,4000)).ok);
    const result=await terminal(id);
    check(op+' signed protocol result',result.ok && result.state==='completed' &&
      result.value===expected);
    check(op+' acknowledged',(await client.ack(id)).ok);
  }
  console.log(JSON.stringify({schema:'tronix.bang.celix-fixture.v1',
    checks, synthetic:true, on_air:false, human_receipt:false},null,2));
} finally {
  if (child.exitCode===null) child.kill('SIGTERM');
  await Promise.race([new Promise(resolve=>child.once('exit',resolve)),delay(2500)]);
  if (child.exitCode===null) child.kill('SIGKILL');
  await rm(dir,{recursive:true,force:true});
}
