// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Opt-in end-to-end OpenThread simulation through native Tronix Celix.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,rm,chmod} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {TronixClient} from '../src/client.mjs';

const root=resolve(import.meta.dirname,'..');
const cli=process.env.OT_CLI_PATH ?? join(root,'.openthread-sim-build/examples/apps/cli/ot-cli-ftd');
const dir=await mkdtemp(join(tmpdir(),'tn-thread-'));
await chmod(dir,0o700);
const bridgeSocket=join(dir,'bridge.sock'),runtimeSocket=join(dir,'runtime.sock');
const children=[];
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
const evidence={schema:'tronix.thread-live.v1',checks:[],nodes:null,leaders:null};
function launch(binary,args,env={}){
  const proc=spawn(binary,args,{cwd:root,env:{...process.env,...env},stdio:['ignore','pipe','pipe']});
  proc.output='';proc.errors='';
  proc.stdout.on('data',chunk=>{proc.output=(proc.output+chunk.toString()).slice(-8192);});
  proc.stderr.on('data',chunk=>{proc.errors=(proc.errors+chunk.toString()).slice(-8192);});
  children.push(proc);return proc;
}
async function ready(proc,signal,limit=60000){
  const start=Date.now();
  while(Date.now()-start<limit){
    if(proc.output.includes(signal))return;
    if(proc.exitCode!==null)throw Error('unexpected exit '+proc.exitCode+' '+proc.errors);
    await sleep(50);
  }
  throw Error('timeout: '+signal+' '+proc.output+' '+proc.errors);
}
const check=(name,ok)=>{assert.ok(ok,name);evidence.checks.push(name);};
async function terminal(client,id){
  for(let i=0;i<80;i++){
    const r=await client.result(id);
    if(['completed','failed','expired'].includes(r.state))return r;
    await sleep(50);
  }
  throw Error('non-terminal job: '+id);
}
try{
  const bridge=launch(process.execPath,['src/thread-bridge.mjs'],{
    OT_CLI_PATH:cli,TRONIX_THREAD_BRIDGE_SOCKET:bridgeSocket,
    THREAD_SIM_BASE_ID:String(Number(process.env.THREAD_SIM_BASE_ID ?? 25))
  });
  await ready(bridge,'THREAD_BRIDGE_READY');
  const daemon=launch(join(root,'build/tronixd'),[
    '--socket',runtimeSocket,'--scheduler',join(root,'build/tronix_scheduler.zip'),
    '--adapter',join(root,'build/tronix_thread.zip')
  ],{TRONIX_THREAD_BRIDGE_SOCKET:bridgeSocket});
  await ready(daemon,'TRONIX_READY');
  const client=new TronixClient(runtimeSocket);
  check('native daemon responds', (await client.ping()).ok);
  const caps=await client.capabilities();
  check('only two authorized Thread operations',caps.ok&&
    caps.operations.length===2 &&
    caps.operations.includes('thread.nodes_up')&&caps.operations.includes('thread.leaders'));
  check('raw CLI denied', (await client.submit('raw','lab','thread.raw_cli')).error==='DENIED');
  for(const [id,op,expected] of [
    ['nodes','thread.nodes_up',2],['leaders','thread.leaders',1]
  ]){
    const submit=await client.submit(id,'lab',op,4000);
    check(op+' accepted',submit.ok);
    const result=await terminal(client,id);
    check(op+' reports '+expected,result.state==='completed'&&result.value===expected);
    check(op+' result acknowledged',(await client.ack(id)).ok);
    if(id==='nodes')evidence.nodes=result.value;else evidence.leaders=result.value;
  }
  bridge.kill('SIGTERM');
  for(let i=0;i<60&&bridge.exitCode===null;i++)await sleep(50);
  check('telemetry bridge terminated',bridge.exitCode!==null);
  const submit=await client.submit('offline','lab','thread.nodes_up',4000);
  check('offline request admitted',submit.ok);
  const result=await terminal(client,'offline');
  check('bridge outage fails closed',result.state==='failed');
  check('failed receipt acknowledged',(await client.ack('offline')).ok);
  console.log(JSON.stringify(evidence,null,2));
}finally{
  for(const child of children)if(child.exitCode===null)child.kill('SIGTERM');
  await sleep(250);
  for(const child of children)if(child.exitCode===null)child.kill('SIGKILL');
  await rm(dir,{recursive:true,force:true});
}
