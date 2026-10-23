// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
import assert from 'node:assert/strict';
import {createHmac,randomBytes,createHash} from 'node:crypto';
import sink from '../cloudflare/checkpoint-worker.mjs';

class D1Fixture {
  rows=[];
  prepare(sql){
    return {bind:(...args)=>({
      run:async()=>{
        assert.match(sql,/INSERT INTO tronix_checkpoints/);
        const [a,g,n,h,payload,hash]=args;
        const prev=this.rows.filter(r=>r.a===a&&r.g===g);
        if(prev.some(r=>r.n>=n))return {meta:{changes:0}};
        this.rows.push({a,g,n,h,payload,payload_sha256:hash});
        return {meta:{changes:1}};
      },
      first:async()=>{
        assert.match(sql,/SELECT seq,payload,payload_sha256/);
        const [a,g,n]=args;
        const items=this.rows.filter(r=>r.a===a&&r.g===g && (n===undefined||r.n===n));
        const chosen=items.sort((x,y)=>y.n-x.n)[0];
        return chosen?{seq:chosen.n,payload:chosen.payload,
          payload_sha256:chosen.payload_sha256}:null;
      }
    })};
  }
}
const key=randomBytes(32);
const publish='publish-'+randomBytes(24).toString('hex');
const read='read-'+randomBytes(24).toString('hex');
const env={DB:new D1Fixture(),CHECKPOINT_HMAC_KEY_HEX:key.toString('hex'),
  PUBLISH_TOKEN:publish,READ_TOKEN:read};
const artifact=randomBytes(32).toString('hex'),grant=randomBytes(32).toString('hex');
const base='https://checkpoint.example';
const path='/v1/checkpoints/'+artifact+'/'+grant;
function make(seq,head=randomBytes(32).toString('hex')) {
  const unsigned={artifactSha256:artifact,chainHead:head,grantSha256:grant,
    schema:'tronix.evidence-checkpoint.v1',sequence:seq};
  const signature=createHmac('sha256',key).update(JSON.stringify(unsigned)).digest('hex');
  return JSON.stringify({...unsigned,signature})+'\n';
}
const post=(raw,token=publish,config=env)=>sink.fetch(new Request(base+'/v1/checkpoints',{
  method:'POST',body:raw,headers:{'authorization':'Bearer '+token,
    'content-type':'application/json'}}),config);
const get=(path,token=read,config=env)=>sink.fetch(new Request(base+path,{
  headers:{'authorization':'Bearer '+token}}),config);
const checks=[],check=(name,condition)=>{assert.ok(condition,name);checks.push(name);};
const one=make(1),same=make(1);
const uploaded=await post(one);
check('signed operator checkpoint accepted',uploaded.status===201);
check('identity binds artifact and grant stream',env.DB.rows.length===1 &&
  env.DB.rows[0].a===artifact && env.DB.rows[0].g===grant);
check('idempotent retry allowed',(await post(one)).status===200);
check('conflicting same-sequence checkpoint refused',(await post(same)).status===409);
const two=make(2),third=make(3);
check('higher sequence accepted',(await post(two)).status===201);
check('valid but older signed checkpoint refused',(await post(one)).status===409);
const latest=await get(path);
check('read-only latest returns exact independently signed bytes',
  latest.status===200 && await latest.text()===two);
check('historical sequence lookup returns exact old checkpoint',
  await (await get(path+'?sequence=1')).text()===one);
check('unknown historical sequence refused',(await get(path+'?sequence=50')).status===404);
check('GET rejects invalid query',(await get(path+'?verbose=1')).status===400);
check('GET rejects malformed sequence',(await get(path+'?sequence=1x')).status===400);
check('GET rejects negative sequence',(await get(path+'?sequence=-1')).status===400);
check('POST unauthorized without publisher token',(await post(third,'invalid')).status===401);
check('GET unauthorized without reader token',(await get(path,'invalid')).status===401);
check('wrong endpoint refused',(await get('/v1/anything')).status===404);
check('signature alteration refused',(await post(two.replace('"sequence":2','"sequence":3'))).status===403);
check('bad canonical field order refused',
  (await post(JSON.stringify(JSON.parse(two))+'   \n')).status===400);
check('checkpoint with injected extra field refused',
  (await post(two.replace('"signature":','"extra":1,"signature":'))).status===400);
const empty={...env,CHECKPOINT_HMAC_KEY_HEX:undefined};
check('missing signing verification key refuses service',(await post(third,publish,empty)).status===503);
const weak={...env,READ_TOKEN:publish};
check('shared read and publish token refuses service',(await post(third,publish,weak)).status===503);
check('unexpected HTTP method denied',
  (await sink.fetch(new Request(base+'/v1/checkpoints',{method:'DELETE'}),env)).status===405);
check('DB failure fails closed',(await post(third,publish,{...env,DB:{
  prepare(){throw Error('simulated database outage');}
}})).status===503);
check('outage does not create a stored checkpoint',env.DB.rows.length===2);
check('post-outage higher sequence still accepted',(await post(third)).status===201);
check('GET returns most recent head after extension',
  await (await get(path)).text()===third);
check('service never returns a receipt key in its response',
  !(await (await get(path)).text()).includes(env.CHECKPOINT_HMAC_KEY_HEX));
console.log(JSON.stringify({schema:'tronix.cloudflare-anchor.v1',outcome:'PASS',
  checks},null,2));
