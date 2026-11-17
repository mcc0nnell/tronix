// SPDX-License-Identifier: Apache-2.0
// Native-backed, owner-only persistent Celix verifier integration tests.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,mkdir,writeFile,chmod,rm,stat} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {encode} from '@msgpack/msgpack';
import {sendVerification,verifyRequest,verifyStats} from '../src/verify_client.mjs';
import {prepareReviewHandoff} from '../src/verify_handoff.mjs';
const root=path.resolve(import.meta.dirname,'..');
const build=process.env.TRONIX_BUILD||path.join(root,'build');
const exe=path.join(build,'tronix_verify_daemon');
const verifier=path.join(build,'tronix_answer_verifier.zip');
const demo=path.join(build,'tronix_demo.zip');
const hash=x=>createHash('sha256').update(x).digest('hex');
const bytes=Buffer.from('42\n');
const expected=hash(bytes);
const empty=hash(Buffer.alloc(0));
const fs=await import('node:fs/promises');
const verifierHash=hash(await fs.readFile(verifier));
const demoHash=hash(await fs.readFile(demo));
let passed=0;
function check(label,fn){return Promise.resolve().then(fn).then(()=>{
  passed++;console.log('PASS',label);
});}
async function launch(args,expectReady=true){
  const child=spawn(exe,args,{stdio:['ignore','pipe','pipe'],env:{PATH:process.env.PATH}});
  let stdout='',stderr='';
  child.stdout.on('data',b=>{stdout+=b.toString();});
  child.stderr.on('data',b=>{stderr+=b.toString();});
  const ready=await new Promise((resolve,reject)=>{
    let complete=false;
    const settle=(err,value)=>{if(complete)return;complete=true;clearTimeout(timer);
      if(err)reject(err);else resolve(value);};
    const timer=setTimeout(()=>settle(Error('startup timeout: '+stderr)),5000);
    child.stdout.on('data',()=>{if(stdout.includes('TRONIX_VERIFY_READY'))settle(null,true);});
    child.once('exit',(code)=>settle(null,false));
  });
  if(expectReady&&!ready)throw Error('unexpected startup failure '+stderr);
  return {child,ready,stdout:()=>stdout,stderr:()=>stderr};
}
async function stopped(child){
  if(child.exitCode!==null)return;
  const exit=new Promise(resolve=>child.once('exit',resolve));
  child.kill('SIGTERM');
  await Promise.race([exit,new Promise((_,reject)=>setTimeout(()=>reject(Error('termination timeout')),5000))]);
}
const dir=await mkdtemp(path.join(tmpdir(),'tronix-typed-test-'));
const sockDir=path.join(dir,'private');
const oracle=path.join(dir,'oracle.txt');
const socket=path.join(sockDir,'typed.sock');
await mkdir(sockDir,{mode:0o700});
await writeFile(oracle,'golden '+expected+'\nempty '+empty+'\n',{mode:0o600});
await chmod(oracle,0o600);
const args=(bundle=verifier,pin=verifierHash)=>[
  '--socket',socket,'--oracle',oracle,'--bundle',bundle,'--bundle-sha256',pin
];
try{
  let server=await launch(args());
  try{
    await check('real Celix service verifies golden hash',async()=>{
      const r=await verifyRequest(socket,'golden',bytes);
      assert.equal(r.ok,true);assert.equal(r.status,'VERIFIED');assert.equal(r.route,'none');
      assert.equal(r.oracleManifestSha256,hash(Buffer.from('golden '+expected+'\nempty '+empty+'\n')));
      assert.equal(r.objectiveTruthEstablished,false);
      assert.equal(r.executionAuthorized,false);
      assert.equal(r.published,false);
    });
    await check('wrong answer is REJECTED, not model-accepted',async()=>{
      const r=await verifyRequest(socket,'golden',Buffer.from('41\n'));
      assert.equal(r.status,'REJECTED');assert.equal(r.route,'none');
    });
    await check('unknown task requires Atlas review, no automatic invocation',async()=>{
      const r=await verifyRequest(socket,'unseen',bytes);
      assert.equal(r.status,'UNVERIFIABLE');assert.equal(r.route,'atlas-review-required');
      assert.equal(r.humanReview,'pending');assert.equal(r.executionAuthorized,false);
    });
    await check('empty binary answer works with independent oracle',async()=>{
      const r=await verifyRequest(socket,'empty',new Uint8Array(0));
      assert.equal(r.status,'VERIFIED');
    });
    await check('oracle is frozen and cannot be changed by later file edits',async()=>{
      await writeFile(oracle,'golden '+hash(Buffer.from('bad'))+'\nempty '+empty+'\n',{mode:0o600});
      const r=await verifyRequest(socket,'golden',bytes);
      assert.equal(r.status,'VERIFIED');assert.equal(r.expectedSha256,expected);
    });
    await check('v1 wire request refused',async()=>{
      const r=await sendVerification(socket,{v:1,action:'verify',taskId:'golden',answer:bytes});
      assert.equal(r.ok,false);assert.equal(r.error,'INVALID_REQUEST');
    });
    await check('unexpected fields refused',async()=>{
      const r=await sendVerification(socket,{v:2,action:'verify',taskId:'golden',answer:bytes,command:'bash'});
      assert.equal(r.ok,false);
    });
    await check('string answer refused',async()=>{
      const r=await sendVerification(socket,{v:2,action:'verify',taskId:'golden',answer:'42'});
      assert.equal(r.ok,false);
    });
    await check('oversized binary answer refused',async()=>{
      const r=await sendVerification(socket,{v:2,action:'verify',taskId:'golden',answer:Buffer.alloc(65537)});
      assert.equal(r.ok,false);
    });
    await check('duplicate MessagePack map keys refused',async()=>{
      const pack=(k,v)=>Buffer.concat([Buffer.from(encode(k)),Buffer.from(encode(v))]);
      const raw=Buffer.concat([
        Buffer.from([0x85]),pack('v',2),pack('action','verify'),pack('taskId','golden'),
        pack('answer',bytes),pack('taskId','golden')
      ]);
      const r=await sendVerification(socket,raw);
      assert.equal(r.ok,false);
    });
    await check('trailing MessagePack data refused',async()=>{
      const raw=Buffer.concat([
        Buffer.from(encode({v:2,action:'verify',taskId:'golden',answer:bytes})),
        Buffer.from(encode(8))]);
      const r=await sendVerification(socket,raw);
      assert.equal(r.ok,false);
    });
    await check('stats exclude malformed requests',async()=>{
      const s=await verifyStats(socket);
      assert.equal(s.ok,true);assert.equal(s.requests,5);
      assert.equal(s.verified,3);assert.equal(s.rejected,1);assert.equal(s.reviewRequired,1);
    });
    let samples=[];
    await check('1000 requests through one persistent Celix service',async()=>{
      for(let i=0;i<1000;i++){
        const t=process.hrtime.bigint();
        const response=await verifyRequest(socket,'golden',bytes);
        samples.push(Number(process.hrtime.bigint()-t)/1e6);
        assert.equal(response.status,'VERIFIED');
      }
      const s=await verifyStats(socket);
      assert.equal(s.requests,1005);assert.equal(s.verified,1003);
    });
    samples.sort((a,b)=>a-b);
    console.log('PERSISTENT_BENCH',JSON.stringify({
      samples:samples.length,medianMs:samples[499],
      p95Ms:samples[949],maxMs:samples[999],
      caveat:'localhost one-at-a-time roundtrip; no LLM provider or Atlas review cost'
    }));
    await check('unknown task prepares inert, owner-private Watcher handoff',async()=>{
      const dirName=path.join(dir,'manual-review');
      const r=await prepareReviewHandoff(socket,'unseen',bytes,dirName);
      assert.equal(r.created,true);assert.equal(r.status,'UNVERIFIABLE');
      const evidence=JSON.parse(await fs.readFile(path.join(dirName,'handoff.json'),'utf8'));
      assert.equal(evidence.schema,'tronix.atlas-review-handoff/v1');
      assert.equal(evidence.answerSha256,hash(bytes));
      assert.equal(evidence.executionAuthorized,false);
      assert.equal(evidence.published,false);
      assert.deepEqual(await fs.readFile(path.join(dirName,'answer.bin')),bytes);
      assert.equal((await stat(dirName)).mode & 0o777,0o700);
    });
    await check('inert handoff refuses to replace existing evidence',async()=>{
      await assert.rejects(prepareReviewHandoff(socket,'unseen',bytes,path.join(dir,'manual-review')));
    });
    await check('known pass does not create Watcher handoff',async()=>{
      const target=path.join(dir,'should-not-exist');
      const r=await prepareReviewHandoff(socket,'golden',bytes,target);
      assert.equal(r.created,false);
      await assert.rejects(stat(target));
    });
    await check('deterministic rejection does not create Watcher handoff',async()=>{
      const target=path.join(dir,'rejection-should-not-exist');
      const r=await prepareReviewHandoff(socket,'golden',Buffer.from('incorrect'),target);
      assert.equal(r.created,false);assert.equal(r.status,'REJECTED');
      await assert.rejects(stat(target));
    });
  }finally{await stopped(server.child);}
  await check('private socket is removed on stop',async()=>{
    await assert.rejects(stat(socket));
  });
  await check('wrong bundle pin fails closed at startup',async()=>{
    const s=await launch(args(verifier,'0'.repeat(64)),false);
    assert.equal(s.ready,false);assert.equal(s.child.exitCode,3);
  });
  await check('wrong Celix bundle without service fails closed',async()=>{
    const s=await launch(args(demo,demoHash),false);
    assert.equal(s.ready,false);assert.equal(s.child.exitCode,3);
  });
  await check('malformed oracle manifest fails closed',async()=>{
    await writeFile(oracle,'golden '+expected+'\ngolden '+expected+'\n',{mode:0o600});
    const s=await launch(args(),false);
    assert.equal(s.ready,false);assert.equal(s.child.exitCode,3);
  });
  await check('world-readable oracle manifest fails closed',async()=>{
    await writeFile(oracle,'golden '+expected+'\n',{mode:0o644});
    await chmod(oracle,0o644);
    const s=await launch(args(),false);
    assert.equal(s.ready,false);assert.equal(s.child.exitCode,3);
  });
  console.log('PERSISTENT_VERIFIER_TESTS',passed,'passed');
} finally {
  await rm(dir,{recursive:true,force:true});
}
