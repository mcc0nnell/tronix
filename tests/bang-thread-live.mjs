// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Signed 6lowBang UDP across TWO real OpenThread POSIX simulated nodes.
// The CLI sends ASCII hex of bounded C wire datagrams so received bytes
// survive the CLI's text-only output. NO physical radio or audio.
import assert from 'node:assert/strict';
import {spawn,execFileSync} from 'node:child_process';
import {performance} from 'node:perf_hooks';
import {resolve,join} from 'node:path';
import {existsSync} from 'node:fs';
import {mkdtemp,mkdir,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {setTimeout as sleep} from 'node:timers/promises';

const root=resolve(import.meta.dirname,'..');
const cli=process.env.OT_CLI_PATH ?? join(root,'.openthread-sim-build/examples/apps/cli/ot-cli-ftd');
const codec=process.env.BANG_THREAD_CODEC ?? join(root,'build/bang-thread-codec');
const base=Number(process.env.THREAD_SIM_BASE_ID ?? 27);
if(!cli.startsWith('/')||!codec.startsWith('/')||!existsSync(cli)||!existsSync(codec)||
   !Number.isInteger(base)||base<1||base>30)
  throw Error('Set absolute OT_CLI_PATH and BANG_THREAD_CODEC; base IDs must be 1..30');
// OpenThread POSIX writes node flash under cwd/tmp. Never reuse flash images
// from a previous trial: stale datasets can mask or break live UDP routing.
const scratch=await mkdtemp(join(tmpdir(),'tronix-bang-ot-'));
await mkdir(join(scratch,'tmp'),{mode:0o700});
class Node {
  constructor(id) {
    this.pending=null;this.buffer='';this.closed=false;this.queue=Promise.resolve();
    this.received=[];
    this.process=spawn(cli,[String(id)],{cwd:scratch,stdio:['pipe','pipe','pipe']});
    this.output='';this.error='';
    this.process.stdout.on('data',data=>this.onData(data.toString()));
    this.process.stderr.on('data',data=>this.error=(this.error+data.toString()).slice(-2000));
    this.process.on('error',e=>this.fail(e));
    this.process.on('exit',()=>this.fail(Error('OpenThread CLI exited: '+this.error)));
  }
  fail(error) {
    this.closed=true;
    if(this.pending){const p=this.pending;this.pending=null;clearTimeout(p.timer);p.reject(error);}
  }
  onData(chunk) {
    this.output=(this.output+chunk).slice(-8192);
    this.buffer+=chunk;
    if(this.buffer.length>8192)this.buffer=this.buffer.slice(-4096);
    let pos;
    while((pos=this.buffer.indexOf('\n'))!==-1){
      const line=this.buffer.slice(0,pos).replace(/\r/g,'').trim().replace(/^>\s*/,'');
      this.buffer=this.buffer.slice(pos+1);
      const received=line.match(/^(\d+) bytes from ([0-9a-fA-F:]+) (\d+) ([0-9a-f]+)$/);
      if(received){
        const [,size,peer,port,hex]=received;
        if(Number(size)===hex.length && Number(port)>0)
          this.received.push({hex,peer,port:Number(port),time:performance.now(),bytes:Number(size)});
        continue;
      }
      if(!this.pending)continue;
      if(line==='Done'){
        const p=this.pending;this.pending=null;clearTimeout(p.timer);p.resolve(p.lines);
      }else if(/^Error \d+:/.test(line)){
        const p=this.pending;this.pending=null;clearTimeout(p.timer);p.reject(Error(p.command+' → '+line));
      }else if(line && line!==this.pending.command)this.pending.lines.push(line);
    }
  }
  command(command,limit=6000) {
    const op=()=>new Promise((resolvePromise,reject)=>{
      if(this.closed) return reject(Error('OpenThread node closed'));
      const timer=setTimeout(()=>{this.pending=null;reject(Error('CLI timeout '+command+' output '+this.output));},limit);
      this.pending={command,timer,resolve:resolvePromise,reject,lines:[]};
      this.process.stdin.write(command+'\n');
    });
    const result=this.queue.then(op);
    this.queue=result.catch(()=>{});
    return result;
  }
  stop(){if(!this.closed)this.process.kill('SIGTERM');}
}
const nodes=[new Node(base),new Node(base+1)];
const evidence={schema:'tronix.bang.thread-posix.v1',simulated:true,on_air:false,checks:[]};
function check(label,ok) {assert.ok(ok,label); evidence.checks.push(label);}
async function waitFor(predicate,limit=40000) {
  const until=performance.now()+limit;
  while(performance.now()<until) {
    try{if(await predicate())return;}catch{}
    await sleep(400);
  }
  throw Error('OpenThread mesh failed to converge');
}
async function state(node){return (await node.command('state'))[0]?.trim();}
async function receiveMatching(node,hex,from,limit=2400) {
  const until=performance.now()+limit;
  while(performance.now()<until) {
    for(let i=from;i<node.received.length;i++)
      if(node.received[i].hex===hex)return node.received[i];
    if(node.closed)throw Error('receiver node exited');
    await sleep(20);
  }
  return null;
}
async function deliverWithRetry(sender,receiver,dest,hex,startIndex) {
  const started=performance.now();
  for(let attempt=1;attempt<=4;attempt++) {
    await sender.command('udp send '+dest+' 49000 '+hex);
    const received=await receiveMatching(receiver,hex,startIndex);
    if(received)return {received,attempts:attempt,elapsed:received.time-started};
  }
  throw Error('No matching OpenThread UDP packet after four attempts: '+receiver.output);
}
try {
  const [leader,peer]=nodes;
  // Explicit fresh dataset, no access to user networks or credentials.
  await leader.command('dataset init new');
  await leader.command('dataset commit active');
  await leader.command('ifconfig up');
  await leader.command('thread start');
  await waitFor(async()=>await state(leader)==='leader');
  const dataset=(await leader.command('dataset active -x'))[0];
  check('fresh, bounded Thread test dataset',/^[0-9a-f]{40,512}$/i.test(dataset));
  await peer.command('dataset set active '+dataset);
  await peer.command('ifconfig up');
  await peer.command('thread start');
  await waitFor(async()=>['child','router'].includes(await state(peer)));
  check('two distinct OpenThread POSIX nodes converged',true);
  const addr=(await peer.command('ipaddr mleid'))[0];
  check('receiver exposes a mesh-local IPv6 address',/^[0-9a-f:]+$/i.test(addr));
  await peer.command('udp open');
  await peer.command('udp bind :: 49000');
  await leader.command('udp open');
  const printed=execFileSync(codec,['emit'],{encoding:'utf8',timeout:5000});
  const first=printed.match(/^FRAME1=([0-9a-f]+)$/m)?.[1];
  const second=printed.match(/^FRAME2=([0-9a-f]+)$/m)?.[1];
  check('native C source generated two bounded signed packets',
    !!first&&!!second&&first.length<=538&&second.length<=538);
  // Allow the newly attached child to stabilize before fragmented UDP.
  await sleep(700);
  // Prove simple UDP reachability before testing multi-frame payloads.
  const probe=await deliverWithRetry(leader,peer,addr,'beef',0);
  check('small UDP datagram received before signed-event traffic',
    probe.received.hex==='beef');
  const secondDelivery=await deliverWithRetry(leader,peer,addr,second,peer.received.length);
  const got2=secondDelivery.received;
  check('packet #2 delivered over simulated OpenThread UDP',got2.hex===second);
  const gap=execFileSync(codec,['verify-gap',got2.hex],{encoding:'utf8',timeout:5000});
  check('withheld caption #1 explicitly reported missing by C stream hub',
    gap.includes('PASS: withheld signed caption #1'));
  const firstDelivery=await deliverWithRetry(leader,peer,addr,first,peer.received.length);
  const got1=firstDelivery.received;
  check('packet #1 delivered over simulated OpenThread UDP',got1.hex===first);
  const result=execFileSync(codec,['verify',got2.hex,got1.hex],{encoding:'utf8',timeout:5000});
  check('C stream hub authenticated, reordered, fanned out and rejected forgeries',
    result.includes('PASS: signed OpenThread UDP events admitted'));
  evidence.udpApplicationBytes=[got2.bytes,got1.bytes];
  evidence.sendAttempts=[secondDelivery.attempts,firstDelivery.attempts];
  evidence.measuredProcessSendToReceiveMs=[
    Math.round(secondDelivery.elapsed*1000)/1000,
    Math.round(firstDelivery.elapsed*1000)/1000
  ];
  evidence.caveats=[
    'Local UDP simulation only; no physical IEEE 802.15.4 RF',
    'Application-byte lengths are CLI ASCII hex, twice the binary codec size',
    'Measurements begin before CLI command dispatch and end at receiver CLI output',
    'Withheld #1 in the gap test is intentional application suppression, not real RF packet loss',
    'Retries are bounded and reported; latency includes time spent retrying',
    'One successful trial is not a reliability or latency distribution',
    'Private captions are not encrypted by end-to-end transport'
  ];
  evidence.outcome='PASS';
  console.log(JSON.stringify(evidence,null,2));
} finally {
  for(const node of nodes)node.stop();
  await sleep(150);
  for(const node of nodes)if(node.process.exitCode===null)node.process.kill('SIGKILL');
  await rm(scratch,{recursive:true,force:true});
}
