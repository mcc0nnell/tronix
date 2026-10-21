// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,rm,writeFile,chmod,symlink} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';

const root=resolve(import.meta.dirname,'..');
const dir=await mkdtemp(join(tmpdir(),'tronix-policy-'));
const uid=process.getuid();
const principal='uid:'+uid;
const checks=[];
const check=(label,assertion)=>{assert.ok(assertion,label);checks.push(label);};
let index=0, daemon=null;
const policy=join(dir,'policy.conf');
const journal=join(dir,'journal.mp');
const buildArgs=[
  '--scheduler',join(root,'build/tronix_scheduler.zip'),
  '--adapter',join(root,'build/tronix_demo.zip')
];

async function writePolicy(text,mode=0o600){
  await writeFile(policy,text,{mode:0o600});
  await chmod(policy,mode);
}
function launch(file=policy,other=[]){
  const socket=join(dir,'daemon-'+(++index)+'.sock');
  const p=spawn(join(root,'build/tronixd'),[
    '--socket',socket,...buildArgs,'--policy',file,...other
  ],{cwd:root,stdio:['ignore','pipe','pipe']});
  let out='',err='';
  p.stdout.on('data',b=>out+=b.toString());
  p.stderr.on('data',b=>err+=b.toString());
  return {p,socket,client:new TronixClient(socket),output:()=>({out,err})};
}
async function ready(instance) {
  for(let i=0;i<200;i++){
    if(instance.output().out.includes('TRONIX_READY')) return;
    if(instance.p.exitCode!==null)throw Error('daemon failed '+JSON.stringify(instance.output()));
    await delay(15);
  }
  throw Error('daemon never ready '+JSON.stringify(instance.output()));
}
async function stopped(p){
  if(p.exitCode!==null)return p.exitCode;
  const closed=new Promise(resolve=>p.once('close',resolve));
  return Promise.race([closed,delay(2000).then(()=>{p.kill('SIGKILL');return closed;})]);
}
async function rejectsStartup(name,file=policy){
  const x=launch(file);
  const exit=await stopped(x.p);
  check(name,exit===2 && !x.output().out.includes('TRONIX_READY'));
}
async function until(client,id){
  for(let i=0;i<150;i++){
    const r=await client.result(id);
    if(['completed','failed','overrun','expired'].includes(r.state))return r;
    await delay(10);
  }
  throw Error('result timeout '+id);
}
try{
  await writePolicy('uid '+uid+'\nallow demo.constant\n');
  daemon=launch(policy,['--journal',journal]);
  await ready(daemon);
  check('strict daemon responds to same-UID peer',(await daemon.client.ping()).ok);
  const caps=await daemon.client.capabilities();
  check('advertised capabilities filtered by owner policy',
    caps.ok&&caps.operations.length===1&&caps.operations[0]==='demo.constant');
  const spoof=await daemon.client.submit('spoof','claimed-admin','demo.constant');
  check('forged agent label rejected',spoof.error==='UNAUTHORIZED');
  const blocked=await daemon.client.submit('blocked',principal,'demo.pause');
  check('adapter-supported but unlisted operation denied',blocked.error==='DENIED');
  check('policy-allowed operation admitted',
    (await daemon.client.submit('allowed',principal,'demo.constant')).ok);
  check('authorized Celix execution returns expected result',
    (await until(daemon.client,'allowed')).value===42);
  check('ack allowed after successful run',(await daemon.client.ack('allowed')).ok);
  const receipt=await daemon.client.request('receipt',{id:'allowed'});
  check('authorized receipt is still durable',
    receipt.ok&&receipt.persisted&&receipt.receipt.state==='acked'&&receipt.receipt.agent===principal);
  const event=await daemon.client.request('events',{after:0,limit:16});
  check('ledger records UID-bound principal rather than invented agent label',
    event.events.length>=3&&event.events.every(x=>x.agent===principal));
  const daemonToStop=daemon;daemon=null;
  daemonToStop.p.kill('SIGTERM');
  check('private policy daemon exits cleanly',(await stopped(daemonToStop.p))===0);

  await chmod(policy,0o644);
  await rejectsStartup('world-readable policy rejected');
  await writePolicy('uid '+uid+'\nallow demo.constant\n');
  const link=join(dir,'linked.conf');
  await symlink(policy,link);
  await rejectsStartup('policy symlink rejected',link);
  await writePolicy('uid '+(uid+1)+'\nallow demo.constant\n');
  await rejectsStartup('policy for another OS principal rejected');
  await writePolicy('uid '+uid+'\nallow demo.constant\nallow demo.constant\n');
  await rejectsStartup('duplicate allowed operation rejected');
  await writePolicy('uid '+uid+'\nallow *\n');
  await rejectsStartup('wildcard operation rejected');
  await writePolicy('uid '+uid+'\nallow demo.constant\nunexpected ignored\n');
  await rejectsStartup('unrecognized directive rejected');
  await writePolicy('uid '+uid+'\n');
  await rejectsStartup('empty capability policy rejected');
  console.log(JSON.stringify({schema:'tronix.peer-policy.v1',outcome:'PASS',checks},null,2));
} catch(e){
  console.error(JSON.stringify({schema:'tronix.peer-policy.v1',outcome:'FAIL',checks,error:String(e)},null,2));
  process.exitCode=1;
} finally {
  if(daemon) {daemon.p.kill('SIGTERM');await stopped(daemon.p);}
  await rm(dir,{recursive:true,force:true});
}
