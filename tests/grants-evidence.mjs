// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {createHash,randomBytes} from 'node:crypto';
import {spawn,spawnSync} from 'node:child_process';
import {mkdtemp,readFile,rm,writeFile,chmod,symlink} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {resolve,join} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';

const root=resolve(import.meta.dirname,'..');
const dir=await mkdtemp(join(tmpdir(),'tronix-grants-'));
const adapter=join(root,'build/tronix_demo.zip');
const sha=createHash('sha256').update(await readFile(adapter)).digest('hex');
const worker=join(root,'build/tronix_worker');
const scheduler=join(root,'build/tronix_scheduler.zip');
const keyPath=join(dir,'hmac.key');
await writeFile(keyPath,randomBytes(32),{mode:0o600});
let index=0,live=null;
const checks=[];
const check=(name,ok)=>{assert.ok(ok,name);checks.push(name);};
const valid=(allow='demo.constant')=>
  'tronix.grants.v1\nbundle org.tronix.adapter.demo\nversion 0.1.0\nartifact '+sha+'\nallow '+allow+'\n';

async function launch(content,{mode=0o600,ref=adapter,options=[],grantPath}={}){
  const n=++index;
  const file=grantPath??join(dir,'grant-'+n+'.conf');
  if(!grantPath){await writeFile(file,content,{mode:0o600});await chmod(file,mode);}
  const socket=join(dir,'sock-'+n+'.sock'), evidence=join(dir,'evidence-'+n+'.txt');
  const args=['--socket',socket,'--scheduler',scheduler,'--adapter',ref,
    '--worker',worker,'--adapter-sha256',sha,
    '--grants',file,'--evidence',evidence,'--evidence-key',keyPath,...options];
  const proc=spawn(join(root,'build/tronixd'),args,{cwd:root,stdio:['ignore','pipe','pipe']});
  let out='',err='';
  proc.stdout.on('data',b=>out+=b.toString());
  proc.stderr.on('data',b=>err+=b.toString());
  const closed=new Promise(resolveClose=>proc.once('close',resolveClose));
  for(let i=0;i<150;i++){
    if(out.includes('TRONIX_READY'))return {ready:true,proc,closed,socket,evidence,
      output:()=>({out,err})};
    if(proc.exitCode!==null||proc.signalCode!==null)break;
    await delay(20);
  }
  const code=await Promise.race([closed,delay(400).then(()=>null)]);
  if(code===null)proc.kill('SIGKILL');
  return {ready:false,proc,closed,code,output:()=>({out,err})};
}
async function stop(x) {
  if(x.proc.exitCode===null&&x.proc.signalCode===null)x.proc.kill('SIGTERM');
  await Promise.race([x.closed,delay(2000)]);
  if(x.proc.exitCode===null&&x.proc.signalCode===null)x.proc.kill('SIGKILL');
}
async function rejects(name,content,options={}){
  const x=await launch(content,options);
  try{check(name,!x.ready&&x.code!==0);}
  finally{await stop(x);}
}
async function terminal(client,id){
  for(let i=0;i<300;i++){
    const result=await client.result(id);
    if(['completed','failed','expired','overrun'].includes(result.state))return result;
    await delay(10);
  }
  throw Error('no terminal status for '+id);
}
function verify(evidence,grant){
  return spawnSync('python3',['scripts/verify_grant_evidence.py','--key',keyPath,
     '--grants',grant,'--adapter',adapter,'--evidence',evidence],{cwd:root,encoding:'utf8'});
}
try{
  await rejects('mismatched approved artifact fails startup',
    valid().replace(sha,'0'.repeat(64)));
  await rejects('wrong bundle name fails startup',
    valid().replace('org.tronix.adapter.demo','org.tronix.adapter.wrong'));
  await rejects('wrong bundle version fails startup',valid().replace('0.1.0','9.0.0'));
  await rejects('unavailable granted operation fails startup',valid('demo.nonexistent'));
  await rejects('duplicate operation grant fails startup',
    valid()+'allow demo.constant\n');
  await rejects('unknown grant directive fails startup',
    valid()+'command execute-all\n');
  await rejects('group-readable grant refused',valid(),{mode:0o644});
  const real=join(dir,'approved-real.conf');
  await writeFile(real,valid(),{mode:0o600});
  const symlinked=join(dir,'linked-grant.conf');
  await symlink(real,symlinked);
  await rejects('symlinked grants refused',valid(),{grantPath:symlinked});

  live=await launch(valid());
  check('valid grants and pinned adapter start supervised runtime',live.ready);
  const client=new TronixClient(live.socket);
  const caps=await client.capabilities();
  check('advertised operations intersect static operator grant',
    caps.ok&&caps.operations.length===1&&caps.operations[0]==='demo.constant');
  check('ungranted bundle operation explicitly denied',
    (await client.submit('denied','fixture','demo.pause',2000)).error==='DENIED');
  check('granted operation admitted',
    (await client.submit('allowed','fixture','demo.constant',2000)).ok);
  const done=await terminal(client,'allowed');
  check('granted operation completes in supervised worker',done.state==='completed'&&done.value===42);
  const record=await readFile(live.evidence,'utf8');
  check('terminal evidence emitted as a single chained record',
    record.split('\n').filter(Boolean).length===1);
  const verified=verify(live.evidence,join(dir,'grant-'+index+'.conf'));
  check('independent Python verifier accepts matching artifact, grant and key',
    verified.status===0&&JSON.parse(verified.stdout).verified===1);

  // Emit a second HMAC-linked record and verify the continuity.
  check('second granted operation admitted',
    (await client.submit('allowed2','fixture','demo.constant',2000)).ok);
  check('second granted execution completes',
    (await terminal(client,'allowed2')).state==='completed');
  const v2=verify(live.evidence,join(dir,'grant-'+index+'.conf'));
  check('independent verifier checks both chained receipts',
    v2.status===0&&JSON.parse(v2.stdout).verified===2);
  const tampered=join(dir,'tampered.txt');
  const original=await readFile(live.evidence,'utf8');
  await writeFile(tampered,original.replace('completed','overrun'));
  check('mutated receipt rejected',verify(tampered,join(dir,'grant-'+index+'.conf')).status!==0);
  const truncated=join(dir,'truncated.txt');
  await writeFile(truncated,original.split('\n')[1]+'\n');
  check('missing chain prefix rejected',
    verify(truncated,join(dir,'grant-'+index+'.conf')).status!==0);
  const keyMutated=join(dir,'wrong.key');
  await writeFile(keyMutated,randomBytes(32),{mode:0o600});
  const wrongKey=spawnSync('python3',['scripts/verify_grant_evidence.py','--key',keyMutated,
    '--grants',join(dir,'grant-'+index+'.conf'),'--adapter',adapter,
    '--evidence',live.evidence],{cwd:root,encoding:'utf8'});
  check('wrong independent verification key rejected',wrongKey.status!==0);

  await stop(live);
  live=null;

  // An over-budget terminal record carries no successful application value.
  live=await launch(valid('demo.overrun'));
  check('only reviewed overrun fixture can start',live.ready);
  const overrunClient=new TronixClient(live.socket);
  check('overrun fixture admitted under separate explicit grant',
    (await overrunClient.submit('over-budget','fixture','demo.overrun',2000)).ok);
  const exceeded=await terminal(overrunClient,'over-budget');
  check('over-budget execution is not represented as completed',
    exceeded.state==='overrun' && !Object.hasOwn(exceeded,'value'));
  const overrunEvidence=await readFile(live.evidence,'utf8');
  check('non-success terminal evidence contains no result value',
    overrunEvidence.includes(' overrun ') && overrunEvidence.includes(' - '));
  const v3=verify(live.evidence,join(dir,'grant-'+index+'.conf'));
  check('independent verifier validates over-budget terminal receipt',
    v3.status===0&&JSON.parse(v3.stdout).verified===1);
  await stop(live);
  live=null;
  console.log(JSON.stringify({schema:'tronix.grants-integration.v1',
    outcome:'PASS',checks},null,2));
}catch(e){
  console.error(JSON.stringify({schema:'tronix.grants-integration.v1',
    outcome:'FAIL',checks,error:String(e),stderr:live?.output().err.slice(-2500)},null,2));
  process.exitCode=1;
}finally{
  if(live)await stop(live);
  await rm(dir,{recursive:true,force:true});
}
