// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Experimental application-level emergency alert delivery over real simulated OpenThread UDP.
// Synthetic alerts only. No CAP/IPAWS integration or certified life-safety behavior.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {performance} from 'node:perf_hooks';
import {isAbsolute, join as pathJoin} from 'node:path';
import {existsSync, mkdtempSync, mkdirSync, rmSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {randomInt} from 'node:crypto';

const cli = process.env.OT_CLI_PATH;
const base = Number(process.env.THREAD_ALERT_BASE_ID ?? 10);
if (!cli || !isAbsolute(cli) || !existsSync(cli) ||
    !Number.isInteger(base) || base < 1 || base > 28) {
  console.error('Set OT_CLI_PATH to an absolute ot-cli-ftd binary; base ID must be 1..28');
  process.exit(2);
}
const portOffset = Number(process.env.THREAD_ALERT_PORT_OFFSET ?? randomInt(20, 500));
if (!Number.isInteger(portOffset) || portOffset < 1 || portOffset > 500) {
  console.error('THREAD_ALERT_PORT_OFFSET must be 1..500');
  process.exit(2);
}
// OpenThread simulator flash lives in a relative tmp/ path. Give every run private backing storage.
const simDir = mkdtempSync(pathJoin(tmpdir(), 'tronix-thread-alert-'));
mkdirSync(pathJoin(simDir, 'tmp'), {mode: 0o700});
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const PORT_ALERT = 61616;
const PORT_ACK = 61617;
const events = [];
const errorEvents = [];
const nodes = [];

class OtNode {
  constructor(name, id) {
    this.name = name;
    this.id = id;
    this.closed = false;
    this.buffer = '';
    this.pending = null;
    this.queue = Promise.resolve();
    this.received = [];
    this.seen = new Set();
    this.activations = [];
    this.onPacket = null;
    this.proc = spawn(cli, [String(id)], {
      cwd: simDir,
      env: {...process.env, PORT_OFFSET: String(portOffset)},
      stdio: ['pipe', 'pipe', 'pipe']
    });
    this.proc.stdout.on('data', chunk => this.feed(chunk.toString()));
    this.proc.stderr.on('data', chunk => {
      errorEvents.push({node: name, stderr: chunk.toString().slice(0, 256)});
    });
    this.proc.on('error', err => this.terminate(err));
    this.proc.on('exit', () => this.terminate(new Error('node exited')));
    this.proc.stdin.on('error', () => {});
  }
  terminate(err) {
    this.closed = true;
    if (this.pending) {
      const p = this.pending;
      this.pending = null;
      clearTimeout(p.timer);
      p.reject(err);
    }
  }
  feed(chunk) {
    this.buffer += chunk;
    if (this.buffer.length > 16384) this.buffer = this.buffer.slice(-4096);
    let idx;
    while ((idx = this.buffer.indexOf('\n')) !== -1) {
      const line = this.buffer.slice(0, idx).replace(/\r/g, '').trim().replace(/^>\s*/, '');
      this.buffer = this.buffer.slice(idx + 1);
      if (process.env.THREAD_ALERT_DEBUG === '1' && (/bytes from/.test(line) || /^Error \d+:/.test(line))) {
        console.error(this.name + ' CLI: ' + line.slice(0, 180));
      }
      const packet = /^\d+ bytes from \S+ \d+ ((?:EA1|ACK1)\|[A-Za-z0-9_-]+\|[A-Za-z0-9_-]+)$/.exec(line);
      if (packet) {
        const item = {node: this.name, body: packet[1], time: performance.now()};
        this.received.push(item);
        if (this.onPacket) Promise.resolve().then(() => this.onPacket(item)).catch(err => {
          errorEvents.push({node: this.name, error: err.message});
        });
        continue;
      }
      const p = this.pending;
      if (!p) continue;
      if (line === 'Done') {
        this.pending = null;
        clearTimeout(p.timer);
        p.resolve(p.lines);
      } else if (/^Error \d+:/.test(line)) {
        this.pending = null;
        clearTimeout(p.timer);
        p.reject(new Error(this.name + ': ' + line + ' [' + p.command.split(' ')[0] + ']'));
      } else if (line && line !== p.command) {
        p.lines.push(line);
      }
    }
  }
  command(command, timeout = 5000) {
    const run = () => new Promise((resolve, reject) => {
      if (this.closed) return reject(new Error(this.name + ' is offline'));
      const timer = setTimeout(() => {
        this.pending = null;
        this.stop();
        reject(new Error(this.name + ' command timeout: ' + command.split(' ')[0]));
      }, timeout);
      this.pending = {resolve, reject, lines: [], timer, command};
      this.proc.stdin.write(command + '\n');
    });
    const result = this.queue.then(run);
    this.queue = result.catch(() => {});
    return result;
  }
  stop() { if (!this.closed) this.proc.kill('SIGTERM'); this.closed = true; }
}

async function until(predicate, timeout = 30000, label = 'condition') {
  const deadline = performance.now() + timeout;
  while (performance.now() < deadline) {
    if (await predicate()) return true;
    await sleep(100);
  }
  if (label === 'optional') return false;
  throw new Error('Timed out waiting for ' + label);
}
async function state(node) { return (await node.command('state'))[0]; }
async function join(node, dataset) {
  await node.command('dataset set active ' + dataset);
  await node.command('ifconfig up');
  await node.command('thread start');
  await until(async () => ['child', 'router'].includes(await state(node)), 30000, node.name + ' attach');
}
async function sendAlert(sender, targets, alertId) {
  const t0 = performance.now();
  const failures = [];
  for (const target of targets) {
    try {
      await sender.command('udp send ' + target.address + ' ' + PORT_ALERT + ' EA1|' + alertId + '|EVACUATE');
    } catch (e) {
      failures.push({target: target.name, error: e.message});
    }
  }
  await until(() => targets.every(n => sender.received.some(e => e.time >= t0 && e.body === 'ACK1|' + alertId + '|' + n.name)),
              2500, 'optional');
  const acked = targets.filter(n => sender.received.some(e => e.time >= t0 && e.body === 'ACK1|' + alertId + '|' + n.name));
  const latency = Object.fromEntries(acked.map(n => {
    const ack = sender.received.find(e => e.time >= t0 && e.body === 'ACK1|' + alertId + '|' + n.name);
    return [n.name, Math.round(ack.time - t0)];
  }));
  return {id: alertId, requested: targets.map(x => x.name), acknowledged: acked.map(x => x.name),
          missing: targets.filter(x => !acked.includes(x)).map(x => x.name),
          ack_latency_ms: latency, send_errors: failures};
}

let finalEvidence;
try {
  const gateway = new OtNode('gateway', base);
  const strobe = new OtNode('strobe', base + 1);
  const display = new OtNode('display', base + 2);
  const haptic = new OtNode('haptic', base + 3);
  nodes.push(gateway, strobe, display, haptic);
  await gateway.command('dataset init new');
  await gateway.command('dataset commit active');
  await gateway.command('ifconfig up');
  await gateway.command('thread start');
  await until(async () => await state(gateway) === 'leader', 30000, 'gateway leader');
  const dataset = (await gateway.command('dataset active -x'))[0];
  assert.match(dataset, /^[a-f0-9]{40,512}$/i);
  for (const node of [strobe, display, haptic]) await join(node, dataset);
  for (const node of nodes) {
    node.address = (await node.command('ipaddr rloc'))[0];
    assert.match(node.address, /^[0-9a-f:]+$/i);
    await node.command('udp open');
    await node.command('udp bind :: ' + (node === gateway ? PORT_ACK : PORT_ALERT));
  }
  // A role transition does not guarantee IPv6 route convergence immediately.
  await sleep(2000);
  if (process.env.THREAD_ALERT_DEBUG === '1') {
    for (const n of nodes) {
      console.error('NETWORK ' + n.name + ' ' + JSON.stringify({
        state: await state(n), address: n.address,
        leaderdata: await n.command('leaderdata'),
        routerTable: n === gateway ? await n.command('router table') : undefined,
        childTable: n === gateway ? await n.command('child table') : undefined,
        mode: await n.command('mode')
      }));
    }
    await gateway.command('ping ' + strobe.address + ' 2');
    await sleep(1200);
  }
  for (const node of [strobe, display, haptic]) {
    node.onPacket = async item => {
      const m = /^EA1\|([A-Za-z0-9_-]+)\|EVACUATE$/.exec(item.body);
      if (!m) return;
      if (!node.seen.has(m[1])) {
        node.seen.add(m[1]);
        node.activations.push(m[1]);
        events.push({node: node.name, event: 'activated', alert_id: m[1]});
      } else {
        events.push({node: node.name, event: 'duplicate_suppressed', alert_id: m[1]});
      }
      await node.command('udp send ' + gateway.address + ' ' + PORT_ACK +
                         ' ACK1|' + m[1] + '|' + node.name);
    };
  }
  const targets = [strobe, display, haptic];
  const baseline = await sendAlert(gateway, targets, 'drill001');
  if (process.env.THREAD_ALERT_DEBUG === '1' && baseline.acknowledged.length !== 3) {
    console.error('BASELINE_FAIL ' + JSON.stringify(baseline));
    for (const n of nodes) {
      console.error('COUNTERS ' + n.name + ' ' + JSON.stringify({
        ip: await n.command('counters ip'), mac: await n.command('counters mac')
      }));
    }
  }
  assert.equal(baseline.acknowledged.length, 3, 'healthy simulated nodes must all ACK');
  const replay = await sendAlert(gateway, [strobe], 'drill001');
  assert.deepEqual(strobe.activations, ['drill001'], 'duplicate alert must not reactivate');
  assert.deepEqual(replay.acknowledged, ['strobe'], 'duplicate still receives ACK');
  haptic.stop();
  await sleep(300);
  const degraded = await sendAlert(gateway, targets, 'drill002');
  assert.deepEqual(degraded.missing, ['haptic'], 'lost node must not be marked delivered');
  assert.equal(degraded.acknowledged.length, 2, 'remaining nodes must ACK');
  finalEvidence = {
    schema: 'tronix.thread-alert-lab.v1',
    transport: 'OpenThread ot-cli-ftd UDP POSIX simulation (no physical radio)',
    payload: 'synthetic EA1 alert and ACK1 receipt; not authenticated CAP',
    baseline, replay, degraded, activations: Object.fromEntries(targets.map(n => [n.name, n.activations])),
    events, errors: errorEvents,
    warning: 'ACK means simulator application receipt, not human acknowledgement or certified alerting'
  };
  console.log(JSON.stringify(finalEvidence, null, 2));
} catch (e) {
  console.error('THREAD_ALERT_LAB_FAILED: ' + e.stack);
  process.exitCode = 1;
} finally {
  for (const node of nodes) node.stop();
  await sleep(150);
  for (const node of nodes) if (node.proc.exitCode === null) node.proc.kill('SIGKILL');
  await sleep(100);
  rmSync(simDir, {recursive: true, force: true});
}
