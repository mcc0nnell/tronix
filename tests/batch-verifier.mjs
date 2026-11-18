// SPDX-License-Identifier: Apache-2.0
// No Celix or npm dependency required: deterministic unit tests for batch admission.
import {test} from 'node:test';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import {verifyBatch} from '../src/verify_batch.mjs';

const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
const GOLD=hash(Buffer.from('gold'));
const manifest='a'.repeat(64);
const entry=(i,answer=Buffer.from('gold'),taskId='golden')=>({candidateId:'c'+i,taskId,answer});
const receipt=(taskId,answer,status='VERIFIED')=>({
  v:2,ok:true,protocol:'tronix.typed-verification/v2',taskId,
  status,route:status==='UNVERIFIABLE'?'atlas-review-required':'none',
  actualSha256:hash(answer),answerBytes:answer.length,
  expectedSha256:status==='UNVERIFIABLE'?'':GOLD,
  oracleManifestSha256:manifest,humanReview:'pending',
  objectiveTruthEstablished:false,executionAuthorized:false,published:false
});

test('bounded fan-out preserves order and captures all three decisions',async()=>{
  const items=Array.from({length:40},(_,i)=>entry(i,
    Buffer.from(i%3===1?'bad':'gold'),i%3===2?'unknown':'golden'));
  let active=0,maxActive=0;
  const verify=async(id,answer)=>{
    active++;maxActive=Math.max(maxActive,active);
    await new Promise(resolve=>setTimeout(resolve,2));
    active--;
    const status=id==='unknown'?'UNVERIFIABLE':
      answer.toString()==='gold'?'VERIFIED':'REJECTED';
    return receipt(id,answer,status);
  };
  const outcomes=await verifyBatch(items,verify,{concurrency:4});
  assert.equal(outcomes.length,40);
  assert.equal(maxActive,4);
  for(let i=0;i<40;i++){
    assert.equal(outcomes[i].candidateId,'c'+i);
    assert.equal(outcomes[i].status,['VERIFIED','REJECTED','UNVERIFIABLE'][i%3]);
    assert.equal(outcomes[i].receipt.executionAuthorized,false);
  }
});

test('preflight rejects invalid or duplicate candidates without dispatch',async()=>{
  let called=0;const verify=async()=>{called++;};
  for(const bad of [
    [entry(0),entry(0)],
    [entry(0),{...entry(1),taskId:'unsafe/operation'}],
    [entry(0),{...entry(1),answer:Buffer.alloc(65537)}],
    Array.from({length:129},(_,i)=>entry(i)),
    Array.from({length:17},(_,i)=>entry(i,Buffer.alloc(65536))),
    []
  ])await assert.rejects(verifyBatch(bad,verify));
  await assert.rejects(verifyBatch([entry(0)],verify,{concurrency:9}));
  assert.equal(called,0);
});

test('input bytes are snapshotted before any asynchronous verification',async()=>{
  const input=Buffer.from('gold');
  const expected=hash(input);
  const result=verifyBatch([entry(1,input)],async(id,answer)=>{
    await new Promise(resolve=>setTimeout(resolve,2));
    assert.equal(hash(answer),expected);
    return receipt(id,answer);
  });
  input.fill(0);
  assert.equal((await result)[0].answerSha256,expected);
});

test('rejects wrong hash, task, authority, status and route as batch failure',async()=>{
  const mutations=[
    r=>({...r,actualSha256:'0'.repeat(64)}),
    r=>({...r,taskId:'other'}),
    r=>({...r,answerBytes:100}),
    r=>({...r,executionAuthorized:true}),
    r=>({...r,published:true}),
    r=>({...r,objectiveTruthEstablished:true}),
    r=>({...r,status:'PROBABLY_VERIFIED'}),
    r=>({...r,route:'atlas-review-required'}),
    r=>({...r,expectedSha256:'0'.repeat(64)})
  ];
  for(const mutate of mutations){
    await assert.rejects(verifyBatch([entry(0)],async(id,answer)=>mutate(receipt(id,answer))));
  }
  await assert.rejects(verifyBatch([entry(0)],async()=>{throw Error('offline');}));
});

test('failure stops new admission; already in-flight work may finish',async()=>{
  const invoked=[];
  const items=Array.from({length:20},(_,i)=>entry(i));
  await assert.rejects(verifyBatch(items,async(id,answer)=>{
    invoked.push(answer);
    if(invoked.length===1)throw Error('bad socket');
    await new Promise(resolve=>setTimeout(resolve,5));
    return receipt(id,answer);
  },{concurrency:3}));
  assert.ok(invoked.length>=1&&invoked.length<=3);
});

test('one candidate allowed, zero-byte answer allowed',async()=>{
  const item=entry(0,Buffer.alloc(0),'unknown');
  const result=await verifyBatch([item],async(id,answer)=>receipt(id,answer,'UNVERIFIABLE'));
  assert.equal(result[0].status,'UNVERIFIABLE');
  assert.equal(result[0].receipt.route,'atlas-review-required');
});
