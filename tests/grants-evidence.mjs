// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {createHash,createHmac,randomBytes} from 'node:crypto';
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
const checkpointKey=join(dir,'operator-only-checkpoint.key');
await writeFile(checkpointKey,randomBytes(32),{mode:0o600});
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
function verify(evidence,grant,extra=[]){
  return spawnSync('python3',['scripts/verify_grant_evidence.py','--key',keyPath,
     '--grants',grant,'--adapter',adapter,'--evidence',evidence,...extra],
    {cwd:root,encoding:'utf8'});
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
  // An operator-owned second key signs a create-only checkpoint after full
  // verification. The test directory models retention; deployment must export
  // this anchor to an independent trust domain.
  const grantPath=join(dir,'grant-'+index+'.conf');
  const checkpointPath=join(dir,'operator-checkpoint-2.json');
  const createAnchor=verify(live.evidence,grantPath,
    ['--write-checkpoint',checkpointPath,'--checkpoint-key',checkpointKey]);
  check('operator creates separate signed checkpoint after verifying native receipts',
    createAnchor.status===0&&JSON.parse(createAnchor.stdout).checkpointMode==='created');
  const anchor=JSON.parse(await readFile(checkpointPath,'utf8'));
  check('signed checkpoint captures exact record count and chain head',
    anchor.sequence===2 && anchor.chainHead===JSON.parse(v2.stdout).chainHead);
  const checked=verify(live.evidence,grantPath,
    ['--checkpoint',checkpointPath,'--checkpoint-key',checkpointKey]);
  check('external checkpoint verifies frozen prefix',
    checked.status===0&&JSON.parse(checked.stdout).anchoredPrefix===2);
  const original=await readFile(live.evidence,'utf8');
  const shorter=join(dir,'complete-but-rolled-back.txt');
  await writeFile(shorter,original.split('\n')[0]+'\n');
  check('previously valid single-record prefix verifies without checkpoint',
    verify(shorter,grantPath).status===0);
  check('independently retained checkpoint detects valid suffix rollback',
    verify(shorter,grantPath,
      ['--checkpoint',checkpointPath,'--checkpoint-key',checkpointKey]).status!==0);
  // A hostile daemon knows its *receipt* key. Even if it can produce a
  // different internally valid chain, the operator-only anchor must reject it.
  const alteredRecords=original.trimEnd().split('\n');
  const changed=alteredRecords[1].split(' ');
  changed[5]='failed';
  changed[7]='-';
  changed[11]=createHmac('sha256',await readFile(keyPath))
    .update(changed.slice(0,11).join(' ')).digest('hex');
  const rewrittenChain=join(dir,'internally-valid-rewritten-chain.txt');
  await writeFile(rewrittenChain,[alteredRecords[0],changed.join(' ')].join('\n')+'\n');
  check('forged chain with known daemon HMAC key passes legacy verification',
    verify(rewrittenChain,grantPath).status===0);
  check('operator checkpoint rejects alternative valid daemon-signed chain',
    verify(rewrittenChain,grantPath,
      ['--checkpoint',checkpointPath,'--checkpoint-key',checkpointKey]).status!==0);
  const beforeOverwrite=await readFile(checkpointPath,'utf8');
  check('create-only checkpoint refuses overwrite',
    verify(live.evidence,grantPath,
      ['--write-checkpoint',checkpointPath,'--checkpoint-key',checkpointKey]).status!==0);
  check('rejected overwrite preserves original external anchor',
    (await readFile(checkpointPath,'utf8'))===beforeOverwrite);
  const wrongOperatorKey=join(dir,'wrong-operator-key');
  await writeFile(wrongOperatorKey,randomBytes(32),{mode:0o600});
  check('incorrect operator-only checkpoint key is rejected',
    verify(live.evidence,grantPath,
      ['--checkpoint',checkpointPath,'--checkpoint-key',wrongOperatorKey]).status!==0);
  check('using daemon receipt key as operator key is forbidden',
    verify(live.evidence,grantPath,
      ['--checkpoint',checkpointPath,'--checkpoint-key',keyPath]).status!==0);
  const mutatedAnchor=join(dir,'corrupted-anchor.json');
  await writeFile(mutatedAnchor,beforeOverwrite.replace('"sequence":2','"sequence":1'));
  check('tampered checkpoint signature is rejected',
    verify(live.evidence,grantPath,
      ['--checkpoint',mutatedAnchor,'--checkpoint-key',checkpointKey]).status!==0);

  check('later valid job is admitted after anchoring',
    (await client.submit('allowed3','fixture','demo.constant',2000)).ok);
  check('later valid job completes after anchoring',
    (await terminal(client,'allowed3')).state==='completed');
  const extended=verify(live.evidence,grantPath,
    ['--checkpoint',checkpointPath,'--checkpoint-key',checkpointKey]);
  check('chain extension preserves checkpointed prefix',
    extended.status===0&&JSON.parse(extended.stdout).verified===3);
  const checkpoint3=join(dir,'operator-checkpoint-3.json');
  const snapshot3=verify(live.evidence,grantPath,
    ['--write-checkpoint',checkpoint3,'--checkpoint-key',checkpointKey]);
  check('second monotonic operator checkpoint created',
    snapshot3.status===0&&JSON.parse(snapshot3.stdout).verified===3);
  const checkBoth=verify(live.evidence,grantPath,
    ['--checkpoint',checkpointPath,'--checkpoint',checkpoint3,
     '--checkpoint-key',checkpointKey]);
  check('multiple independently signed checkpoints verify',
    checkBoth.status===0&&JSON.parse(checkBoth.stdout).checkpointCount===2);
  const twoRecords=join(dir,'two-anchored-only.txt');
  await writeFile(twoRecords,(await readFile(live.evidence,'utf8')).split('\n')
    .slice(0,2).join('\n')+'\n');
  check('checkpoint cannot detect rollback entirely after its last anchor',
    verify(twoRecords,grantPath,
      ['--checkpoint',checkpointPath,'--checkpoint-key',checkpointKey]).status===0);
  check('later checkpoint detects rollback that earlier anchor allows',
    verify(twoRecords,grantPath,
      ['--checkpoint',checkpoint3,'--checkpoint-key',checkpointKey]).status!==0);
  const tampered=join(dir,'tampered.txt');
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
