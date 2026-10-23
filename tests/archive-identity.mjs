// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Operator pinning: pre-activation denial and immutable runtime archive snapshot.
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {spawn} from 'node:child_process';
import {copyFile, mkdtemp, readFile, rm, symlink, writeFile} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join, resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';

const root=resolve(import.meta.dirname,'..');
const dir=await mkdtemp(join(tmpdir(),'tronix-identity-'));
const daemon=join(root,'build/tronixd');
const adapter=join(root,'build/tronix_demo.zip');
const scheduler=join(root,'build/tronix_scheduler.zip');
const worker=join(root,'build/tronix_worker');
const pin=createHash('sha256').update(await readFile(adapter)).digest('hex');
const checks=[];
const check=(name,ok)=>{assert.ok(ok,name);checks.push(name);};
let serial=0;

async function start(archive,digest,{supervised=false}={}){
  const socket=join(dir,'runtime-'+(++serial)+'.sock');
  const args=['--socket',socket,'--scheduler',scheduler,'--adapter',archive,
    '--adapter-sha256',digest];
  if(supervised)args.push('--worker',worker);
  const proc=spawn(daemon,args,{cwd:root,stdio:['ignore','pipe','pipe']});
  let out='',err='';
  proc.stdout.on('data',b=>{out+=b.toString();});
  proc.stderr.on('data',b=>{err+=b.toString();});
  const exit=new Promise(resolveResult=>proc.once('close',resolveResult));
  for(let i=0;i<200;i++){
    if(out.includes('TRONIX_READY'))return {proc,socket,out,err,exit,ready:true};
    if(proc.exitCode!==null || proc.signalCode!==null)break;
    await delay(20);
  }
  const code=await Promise.race([exit,delay(300).then(()=>null)]);
  if(code===null)proc.kill('SIGKILL');
  return {proc,socket,out,err,exit,ready:false,code};
}
async function stop(daemonState){
  if(daemonState.proc.exitCode===null && daemonState.proc.signalCode===null)
    daemonState.proc.kill('SIGTERM');
  await Promise.race([daemonState.exit,delay(2000)]);
  if(daemonState.proc.exitCode===null && daemonState.proc.signalCode===null)
    daemonState.proc.kill('SIGKILL');
}
async function runOne(state,id){
  const client=new TronixClient(state.socket);
  check('pinned runtime exposes approved operation '+id,
    (await client.capabilities()).operations.includes('demo.constant'));
  check('pinned runtime admits approved operation '+id,
    (await client.submit(id,'trusted-agent','demo.constant',2000)).ok);
  let result;
  for(let i=0;i<300;i++){
    result=await client.result(id);
    if(['completed','failed','expired','overrun'].includes(result.state))break;
    await delay(10);
  }
  check('pinned approved operation completes '+id,
    result?.state==='completed' && result.value===42);
  await client.ack(id);
}
try {
  const good=join(dir,'approved.zip');
  await copyFile(adapter,good);
  const unpinnedTampered=join(dir,'tampered.zip');
  await writeFile(unpinnedTampered,'not the reviewed Celix archive');
  const wrong=await start(unpinnedTampered,pin);
  check('changed bytes denied before startup',!wrong.ready && wrong.code!==0);
  await stop(wrong);

  const malformed=await start(good,'z'.repeat(64));
  check('malformed operator digest rejected',!malformed.ready && malformed.code!==0);
  await stop(malformed);

  // Exercise the independent execution-child pin, not merely daemon startup.
  const workerCache=await mkdtemp(join(tmpdir(),'tronix-worker-'));
  try {
    const direct=spawn(worker,['--adapter',good,'--operation','demo.constant',
      '--cache',workerCache,'--lane','1','--budget-ms','1000',
      '--adapter-sha256','0'.repeat(64)],
      {cwd:root,stdio:['ignore','ignore','pipe','pipe']});
    let directErr='';
    direct.stderr.on('data',part=>{directErr+=part.toString();});
    const childCode=await new Promise(resolveCode=>direct.once('close',resolveCode));
    check('standalone child refuses mismatched pin before Celix activation',
      childCode!==0 && directErr.includes('adapter identity verification failed'));
  }finally {await rm(workerCache,{recursive:true,force:true});}

  const link=join(dir,'linked.zip');
  await symlink(good,link);
  const linked=await start(link,pin);
  check('symbolic link rejected rather than followed',!linked.ready && linked.code!==0);
  await stop(linked);

  for(const supervised of [false,true]){
    await copyFile(adapter,good);
    const state=await start(good,pin,{supervised});
    check('valid pin starts '+(supervised?'supervised':'inprocess'),
      state.ready);
    try {
      await runOne(state,supervised?'supervised-before':'inprocess-before');
      await writeFile(good,'changed original after verified snapshot');
      await runOne(state,supervised?'supervised-after':'inprocess-after');
    } finally {await stop(state);}
  }
  console.log(JSON.stringify({schema:'tronix.adapter-identity.v1',outcome:'PASS',checks},null,2));
}catch(error){
  console.error(JSON.stringify({schema:'tronix.adapter-identity.v1',outcome:'FAIL',
    checks,error:String(error)},null,2));
  process.exitCode=1;
}finally{await rm(dir,{recursive:true,force:true});}
