// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Optional Celix caption-service registration smoke. Does not transport text
// via Tronix's existing integer-returning job/MessagePack API.
import assert from 'node:assert/strict';
import {existsSync} from 'node:fs';
import {mkdtemp,rm,chmod} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {spawn} from 'node:child_process';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';
const root=resolve(import.meta.dirname,'..');
const bin=join(root,'build/tronixd');
const bundle=join(root,'build/tronix_bang_stream.zip');
const scheduler=join(root,'build/tronix_scheduler.zip');
const adapter=join(root,'build/tronix_demo.zip');
for(const path of [bin,bundle,scheduler,adapter])
  if(!existsSync(path))throw Error('Missing native build output: '+path);
const directory=await mkdtemp(join(tmpdir(),'tronix-caption-service-'));
await chmod(directory,0o700);
const socket=join(directory,'runtime.sock');
const args=['--socket',socket,'--scheduler',scheduler,'--adapter',adapter,
  '--bang-stream',bundle];
function spawnDaemon(env) {
  const proc=spawn(bin,args,{cwd:root,env,stdio:['ignore','pipe','pipe']});
  proc.stdoutText='';proc.stderrText='';
  proc.stdout.on('data',c=>proc.stdoutText=(proc.stdoutText+c).slice(-8192));
  proc.stderr.on('data',c=>proc.stderrText=(proc.stderrText+c).slice(-8192));
  return proc;
}
async function exit(proc) {
  for(let n=0;n<100;n++) {
    if(proc.exitCode!==null)return proc.exitCode;
    await delay(20);
  }
  throw Error('process did not exit after invalid fixture configuration');
}
async function ready(proc) {
  for(let n=0;n<200;n++) {
    if(proc.stdoutText.includes('TRONIX_BANG_STREAM_READY'))return;
    if(proc.exitCode!==null)throw Error('service startup failed '+proc.exitCode+' '+proc.stderrText);
    await delay(25);
  }
  throw Error('caption Celix service not registered '+proc.stderrText);
}
const checks=[];
const check=(name,yes)=>{assert.ok(yes,name);checks.push(name);};
let daemon;
try{
  const invalid=spawnDaemon({...process.env,TRONIX_BANG_STREAM_ENABLE_FIXTURE:'0'});
  check('stream requires explicit fixture enable',await exit(invalid)===2);
  daemon=spawnDaemon({...process.env,TRONIX_BANG_STREAM_ENABLE_FIXTURE:'1'});
  await ready(daemon);
  const client=new TronixClient(socket);
  check('existing normal IPC remains accessible',(await client.ping()).ok);
  const caps=await client.capabilities();
  check('existing numeric demo operation remains intact',
    caps.ok && caps.operations.length>=1 && caps.operations.includes('demo.constant') &&
    !caps.operations.includes('bang.synthetic_delivery'));
  const forbidden=await client.submit('forbidden','fixture','bang.caption_payload',1000);
  check('caption text injection via old API is denied',forbidden.error==='DENIED');
  const a=await client.submit('normal','fixture','demo.constant',2000);
  check('normal Celix operation still admitted',a.ok);
  let result;
  for(let n=0;n<100;n++){
    result=await client.result('normal');
    if(['completed','failed','overrun','expired'].includes(result.state))break;
    await delay(20);
  }
  check('existing scheduler still returns a numeric result',result?.state==='completed' && result.value===42);
  check('numeric receipt acknowledgment still works',(await client.ack('normal')).ok);
  console.log(JSON.stringify({schema:'tronix.bang.stream-registration.v1',
    checks,caption_text_via_ipc:false,radio_tested:false},null,2));
}finally{
  if(daemon && daemon.exitCode===null)daemon.kill('SIGTERM');
  if(daemon) {
    await Promise.race([new Promise(done=>daemon.once('exit',done)),delay(2500)]);
    if(daemon.exitCode===null)daemon.kill('SIGKILL');
  }
  await rm(directory,{recursive:true,force:true});
}
