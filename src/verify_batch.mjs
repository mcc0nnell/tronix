// SPDX-License-Identifier: Apache-2.0
// Bounded client-side fan-out for TRONIX typed verification. No execution authority.
import {createHash} from 'node:crypto';

const ID=/^[A-Za-z0-9_-]{1,64}$/;
const SHA=/^[0-9a-f]{64}$/;
const MAX_ITEMS=128;
const MAX_BYTES=65536;
const MAX_TOTAL_BYTES=1048576;
const MAX_IN_FLIGHT=8;

function requireReceipt(receipt,taskId,answerSha256,answerBytes){
  if(!receipt||typeof receipt!=='object'||Array.isArray(receipt)||
     receipt.v!==2||receipt.ok!==true||
     receipt.protocol!=='tronix.typed-verification/v2'||
     receipt.taskId!==taskId||receipt.actualSha256!==answerSha256||
     receipt.answerBytes!==answerBytes||!SHA.test(receipt.oracleManifestSha256)||
     receipt.executionAuthorized!==false||receipt.published!==false||
     receipt.objectiveTruthEstablished!==false||receipt.humanReview!=='pending')
    throw Error('invalid or unbound typed verification receipt');
  if(receipt.status==='VERIFIED'){
    if(receipt.route!=='none'||receipt.expectedSha256!==answerSha256)
      throw Error('contradictory verified receipt');
  }else if(receipt.status==='REJECTED'){
    if(receipt.route!=='none'||!SHA.test(receipt.expectedSha256)||
       receipt.expectedSha256===answerSha256)
      throw Error('contradictory rejected receipt');
  }else if(receipt.status==='UNVERIFIABLE'){
    if(receipt.route!=='atlas-review-required'||
       !(receipt.expectedSha256===''||SHA.test(receipt.expectedSha256)))
      throw Error('contradictory unverifiable receipt');
  }else throw Error('unknown verification decision');
  return receipt.status;
}

/**
 * Verify a finite batch against a caller-supplied, pinned TRONIX verifier.
 *
 * The only side effect is invoking the supplied verify(taskId, bytes) callback;
 * this module never invokes Atlas Watcher, a model, a shell, or an adapter.
 *
 * Every input is preflight-validated and snapshotted before the first call.
 * On a malformed receipt or transport failure, reject the WHOLE batch; in-flight
 * requests finish but no new calls are dispatched once failure is observed.
 * Results preserve input order. No deduplication or implicit retries occur.
 */
export async function verifyBatch(items,verify,{concurrency=4}={}){
  if(typeof verify!=='function')throw TypeError('verifier callback required');
  if(!Number.isInteger(concurrency)||concurrency<1||concurrency>MAX_IN_FLIGHT)
    throw RangeError('concurrency must be 1..8');
  if(!Array.isArray(items)||items.length<1||items.length>MAX_ITEMS)
    throw RangeError('batch must contain 1..128 candidates');
  let total=0;
  const seen=new Set();
  const inputs=items.map((item)=>{
    if(!item||typeof item!=='object'||typeof item.candidateId!=='string'||
       typeof item.taskId!=='string'||!ID.test(item.candidateId)||
       !ID.test(item.taskId)||!(item.answer instanceof Uint8Array)||
       item.answer.byteLength>MAX_BYTES||seen.has(item.candidateId))
      throw Error('invalid, oversized or duplicate candidate');
    seen.add(item.candidateId);
    total+=item.answer.byteLength;
    if(total>MAX_TOTAL_BYTES)throw RangeError('batch byte budget exceeded');
    // Buffer.from(Uint8Array) copies; caller mutations cannot change verified bytes.
    const answer=Buffer.from(item.answer);
    return {candidateId:item.candidateId,taskId:item.taskId,answer,
      hash:createHash('sha256').update(answer).digest('hex')};
  });
  const results=new Array(inputs.length);
  let cursor=0;
  let failure;
  async function worker(){
    while(!failure){
      const index=cursor++;
      if(index>=inputs.length)return;
      const item=inputs[index];
      try{
        const receipt=await verify(item.taskId,item.answer);
        const status=requireReceipt(receipt,item.taskId,item.hash,item.answer.length);
        results[index]=Object.freeze({
          candidateId:item.candidateId,taskId:item.taskId,
          answerSha256:item.hash,status,receipt
        });
      }catch(error){
        if(!failure)failure=Error('batch verification failed for '+item.candidateId+': '+String(error));
      }
    }
  }
  await Promise.all(Array.from({length:Math.min(concurrency,inputs.length)},()=>worker()));
  if(failure)throw failure;
  return results;
}
