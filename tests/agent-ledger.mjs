// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,rm,stat,readFile,unlink} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';

const root=resolve(import.meta.dirname,'..');
const dir=await mkdtemp(join(tmpdir(),'tronix-ledger-'));
const sock=join(dir,'daemon.sock');
const journal=join(dir,'events.mp');
const client=new TronixClient(sock);
const checks=[];
const check=(name,value)=>{assert.ok(value,name);checks.push(name);};
let daemon;
async function start(){
  const p=spawn(join(root,'build/tronixd'),[
    '--socket',sock,'--scheduler',join(root,'build/tronix_scheduler.zip'),
    '--adapter',join(root,'build/tronix_demo.zip'),'--journal',journal
  ],{cwd:root,stdio:['ignore','pipe','pipe']});
  let out='',err='';
  p.stdout.on('data',b=>{out+=b.toString();});
  p.stderr.on('data',b=>{err+=b.toString();});
  for(let i=0;i<250;i++){
    if(out.includes('TRONIX_READY')){daemon=p;return;}
    if(p.exitCode!==null)throw Error('journal daemon exit '+p.exitCode+' '+err);
    await delay(10);
  }
  p.kill('SIGKILL');
  throw Error('journal daemon never ready '+err);
}
async function stop(signal='SIGTERM'){
  const p=daemon;
  const closed=new Promise(resolve=>p.once('close',resolve));
  p.kill(signal);
  const exit=await Promise.race([closed,delay(2000).then(()=>null)]);
  if(exit===null){p.kill('SIGKILL');await closed;}
  daemon=null;
  if(signal==='SIGKILL')await unlink(sock); // owned fixture socket survives forced exit
}
async function until(id,terminal=['completed','failed','overrun','expired']){
  for(let i=0;i<150;i++){
    const r=await client.result(id);
    if(terminal.includes(r.state))return r;
    await delay(5);
  }
  throw Error('job timed out '+id);
}
try{
  await start();
  check('journal created private',((await stat(journal)).mode&0o077)===0);
  const malformed=await client.request('events',{after:0,limit:4,operation:'demo.constant'});
  check('events rejects extra untrusted fields',malformed.error==='BAD_REQUEST');
  const malformedLimit=await client.request('events',{after:0,limit:17});
  check('events pagination bounded',malformedLimit.error==='BAD_REQUEST');
  check('trusted operation admitted',(await client.submit('ledger-1','alpha','demo.constant')).ok);
  const completed=await until('ledger-1');
  check('native completion still returns correct value',completed.value===42);
  let ev=await client.request('events',{after:0,limit:16});
  check('lifecycle ordered queued / running / completed',
        ev.persisted===true&&
        ev.events.map(x=>x.state).join(',')==='queued,running,completed'&&
        ev.events.every((x,i)=>x.seq===i+1));
  check('receipt survives native ack',(await client.ack('ledger-1')).ok);
  const acknowledged=await client.request('receipt',{id:'ledger-1'});
  check('acknowledged receipt retains measured outcome',
        acknowledged.ok && acknowledged.persisted &&
        acknowledged.receipt.state==='acked' &&
        acknowledged.receipt.value===42 &&
        acknowledged.receipt.dispatchSequence===completed.dispatchSequence);
  const beforeRestart=(await client.request('events',{after:2,limit:16}));
  check('cursor pagination omits earlier events',
        beforeRestart.events[0]?.seq===3 && beforeRestart.events.length===2);
  await stop();
  await start();
  const recovered=await client.request('receipt',{id:'ledger-1'});
  check('durable receipt replayed across daemon restart',
        recovered.ok && recovered.receipt.state==='acked'&&recovered.receipt.value===42);
  check('durable IDs cannot silently reuse a prior run',
        (await client.submit('ledger-1','alpha','demo.constant')).error==='DUPLICATE_ID');
  const ping=await client.ping();
  check('restarted Celix daemon healthy',ping.ok);
  const previous=(await client.request('events',{after:0,limit:16}));
  check('event sequence survives restart',previous.lastSeq===4);
  // Kill the daemon while executing: replay must label unknown result interrupted,
  // and MUST NOT re-run the operation.
  check('slow fixture admitted',(await client.submit('crash-run','alpha','demo.slow')).ok);
  for(let i=0;i<100;i++){
    if((await client.result('crash-run')).state==='running')break;
    await delay(2);
  }
  check('operation was running before induced crash',
        (await client.result('crash-run')).state==='running');
  await stop('SIGKILL');
  await start();
  const interrupted=await client.request('receipt',{id:'crash-run'});
  check('interrupted run marked explicitly, not reported as success',
        interrupted.ok && interrupted.receipt.state==='interrupted');
  check('crash recovery does not redispatch original job',
        (await client.result('crash-run')).error==='NOT_FOUND');
  const seq=(await client.request('events',{after:0,limit:16}));
  check('recovery appended sequenced interruption event',
        seq.events.at(-1).state==='interrupted'&&seq.events.at(-1).seq===seq.lastSeq);
  check('trusted overrun fixture admitted',
        (await client.submit('overrun-run','alpha','demo.overrun')).ok);
  const overrun=await until('overrun-run');
  check('over-budget execution remains a terminal failure',
        overrun.state==='overrun' && !Object.hasOwn(overrun,'value'));
  const overrunReceipt=await client.receipt('overrun-run');
  check('overrun receipt persists but cannot invent successful value',
        overrunReceipt.receipt.state==='overrun' &&
        !Object.hasOwn(overrunReceipt.receipt,'value'));
  await client.ack('overrun-run');

  check('blocking fixture admitted',
        (await client.submit('block-expiry','alpha','demo.slow')).ok);
  for(let i=0;i<100;i++){
    if((await client.result('block-expiry')).state==='running')break;
    await delay(2);
  }
  check('short-lived pending operation admitted',
        (await client.submit('event-expiry','beta','demo.constant',1)).ok);
  await delay(20);
  const expiryEvents=await client.events(0,16);
  check('event poll reconciles queued expiration without result polling',
        expiryEvents.events.some(e=>e.id==='event-expiry'&&e.state==='expired'));
  await until('block-expiry');
  await client.ack('block-expiry');
  await client.ack('event-expiry');

  // A second process may not open the same append-only journal concurrently.
  const second=spawn(join(root,'build/tronixd'),[
    '--socket',join(dir,'other.sock'),
    '--scheduler',join(root,'build/tronix_scheduler.zip'),
    '--adapter',join(root,'build/tronix_demo.zip'),
    '--journal',journal
  ],{cwd:root,stdio:['ignore','pipe','pipe']});
  let otherErr='';
  second.stderr.on('data',b=>{otherErr+=b.toString();});
  const duplicateWriter=await Promise.race([
    new Promise(resolve=>second.once('close',resolve)),
    delay(2000).then(()=>null)
  ]);
  if(duplicateWriter===null)second.kill('SIGKILL');
  check('second journal writer is refused',
        duplicateWriter===2 && otherErr.includes('already held'));
  // Ensure no unauthorized broad tool capability appears.
  check('capabilities remain restricted',
        !(await client.capabilities()).operations.includes('bash'));
  check('journal actually holds bytes',(await readFile(journal)).length>100);
  console.log(JSON.stringify({schema:'tronix.agent-ledger.v1',outcome:'PASS',checks},null,2));
}catch(e){
  console.error(JSON.stringify({schema:'tronix.agent-ledger.v1',outcome:'FAIL',checks,error:String(e)},null,2));
  process.exitCode=1;
}finally{
  if(daemon)await stop('SIGKILL');
  await rm(dir,{recursive:true,force:true});
}
