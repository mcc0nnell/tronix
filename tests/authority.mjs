// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Linux SO_PEERCRED authority integration. All calls use the same OS UID.
// Verifies fail-closed policy mapping, not same-UID process isolation.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,writeFile,chmod,rm,symlink} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';

const root=resolve(import.meta.dirname,'..');
const dir=await mkdtemp(join(tmpdir(),'tronix-authority-'));
const sock=join(dir,'runtime.sock');
const journal=join(dir,'events.mp');
const memory=join(dir,'memory.mp');
const policy=join(dir,'authority.policy');
const uid=process.getuid();
const client=new TronixClient(sock);
const checks=[];
const check=(name,result)=>{assert.ok(result,name);checks.push(name);};
let daemon=null;
async function start(options=[]) {
  const p=spawn(join(root,'build/tronixd'),[
    '--socket',sock,'--scheduler',join(root,'build/tronix_scheduler.zip'),
    '--adapter',join(root,'build/tronix_demo.zip'),
    '--journal',journal,'--memory',memory,
    '--worker',join(root,'build/tronix_worker'),...options
  ],{cwd:root,stdio:['ignore','pipe','pipe']});
  let stdout='',stderr='';
  p.stdout.on('data',b=>stdout+=b.toString());
  p.stderr.on('data',b=>stderr+=b.toString());
  for(let i=0;i<250;i++) {
    if(stdout.includes('TRONIX_READY')){daemon=p;return;}
    if(p.exitCode!==null)throw Error('daemon stopped '+p.exitCode+': '+stderr);
    await delay(10);
  }
  p.kill('SIGKILL');
  throw Error('daemon not ready: '+stderr);
}
async function stop() {
  const p=daemon;
  if(!p)return;
  const closed=new Promise(resolve=>p.once('close',resolve));
  p.kill('SIGTERM');
  if((await Promise.race([closed,delay(2000).then(()=>null)]))===null){
    p.kill('SIGKILL');await closed;
  }
  daemon=null;
}
async function until(id) {
  for(let i=0;i<200;i++) {
    const r=await client.result(id);
    if(['completed','failed','overrun','expired'].includes(r.state))return r;
    await delay(5);
  }
  throw Error('timeout waiting '+id);
}
async function refused(configPath) {
  const p=spawn(join(root,'build/tronixd'),[
    '--socket',join(dir,'bad-'+String(Math.random()).slice(2)+'.sock'),
    '--scheduler',join(root,'build/tronix_scheduler.zip'),
    '--adapter',join(root,'build/tronix_demo.zip'),
    '--authority',configPath
  ],{cwd:root,stdio:['ignore','pipe','pipe']});
  let stderr='';
  p.stderr.on('data',b=>stderr+=b.toString());
  const code=await Promise.race([
    new Promise(resolve=>p.once('close',resolve)),
    delay(2500).then(()=>null)
  ]);
  if(code===null)p.kill('SIGKILL');
  return code===2 && stderr.includes('authority initialization failed');
}
try {
  // Seed another agent's durable data in the legacy lab mode.
  await start();
  check('seed unrelated agent memory',(await client.memoryPut('other-agent','secret','hidden')).ok);
  check('seed owner memory',(await client.memoryPut('trusted-agent','status','ready')).ok);
  check('seed unrelated agent job',(await client.submit('foreign-id','other-agent','demo.constant')).ok);
  check('unrelated agent job completed',(await until('foreign-id')).state==='completed');
  await stop();

  await writeFile(policy,String(uid)+' trusted-agent demo.constant,demo.pause\n',{mode:0o600});
  await start(['--authority',policy]);
  check('credential-bound client can ping',(await client.ping()).ok);
  const caps=await client.capabilities();
  check('capabilities filtered to configured allowlist',
        caps.operations.length===2 &&
        caps.operations.includes('demo.constant') &&
        caps.operations.includes('demo.pause') &&
        !caps.operations.includes('demo.slow'));

  check('client cannot claim arbitrary agent label',
        (await client.submit('fake','other-agent','demo.constant')).error==='DENIED');
  check('client cannot submit ungranted tool',
        (await client.submit('denied-tool','trusted-agent','demo.slow')).error==='DENIED');
  check('client cannot submit arbitrary commands',
        (await client.submit('shell','trusted-agent','shell.exec')).error==='DENIED');

  check('memory namespace impersonation denied',
        (await client.memoryGet('other-agent','secret')).error==='DENIED');
  check('foreign memory writes denied',
        (await client.memoryPut('other-agent','secret','overwritten')).error==='DENIED');
  check('foreign memory enumeration denied',
        (await client.memoryList('other-agent')).error==='DENIED');
  check('foreign memory deletion denied',
        (await client.memoryDelete('other-agent','secret')).error==='DENIED');
  check('trusted agent can read its persistent memory',
        (await client.memoryGet('trusted-agent','status')).value==='ready');
  check('trusted agent can update its memory',
        (await client.memoryPut('trusted-agent','status','active')).ok);
  check('trusted agent lists only its keys',
        (await client.memoryList('trusted-agent')).keys.join(',')==='status');

  check('foreign durable receipt unavailable',
        (await client.receipt('foreign-id')).error==='NOT_FOUND');
  check('global snapshot prohibited in UID-bound mode',
        (await client.snapshot()).error==='DENIED');
  const initial=await client.events();
  check('foreign lifecycle hidden from events',initial.events.length===0);

  check('bound owner can submit approved operation',
        (await client.submit('mine','trusted-agent','demo.constant')).ok);
  const finished=await until('mine');
  check('trusted result returned',finished.state==='completed'&&finished.value===42);
  const ownEvents=await client.events();
  check('only matching agent lifecycle visible',
        ownEvents.events.length>=3 &&
        ownEvents.events.every(e=>e.agent==='trusted-agent'));
  check('own receipt visible',(await client.receipt('mine')).receipt.agent==='trusted-agent');
  check('own receipt can be acknowledged',(await client.ack('mine')).ok);
  await stop();

  // A peer UID absent from the frozen policy cannot claim the mapped identity.
  await writeFile(policy,String(uid+1)+' trusted-agent demo.constant\n',{mode:0o600});
  await start(['--authority',policy]);
  check('unlisted SO_PEERCRED UID denied',
        (await client.ping()).error==='DENIED' &&
        (await client.capabilities()).error==='DENIED');
  await stop();
  // A policy owned by the daemon but group-readable is rejected.
  await writeFile(policy,String(uid)+' trusted-agent demo.constant\n',{mode:0o600});
  await chmod(policy,0o644);
  check('group-readable authority file refused',await refused(policy));
  await chmod(policy,0o600);
  await writeFile(policy,String(uid)+' trusted-agent demo.constant\n'+
                         String(uid)+' second-agent demo.pause\n',{mode:0o600});
  check('duplicate UID authority refused',await refused(policy));
  await writeFile(policy,String(uid)+' trusted-agent demo.constant,\n',{mode:0o600});
  check('trailing comma authority refused',await refused(policy));
  await writeFile(policy,String(uid)+' trusted-agent demo.constant\n',{mode:0o600});
  await symlink(policy,join(dir,'policy-link'));
  check('symlinked authority file refused',await refused(join(dir,'policy-link')));
  console.log(JSON.stringify({schema:'tronix.uid-authority.v1',outcome:'PASS',checks},null,2));
}catch(e){
  console.error(JSON.stringify({schema:'tronix.uid-authority.v1',outcome:'FAIL',checks,
    error:String(e)},null,2));
  process.exitCode=1;
}finally{
  await stop();
  await rm(dir,{recursive:true,force:true});
}
