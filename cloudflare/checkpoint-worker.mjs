// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// D1 enforces monotonic publication; account administrators can still change D1.
const utf8 = new TextEncoder();
const hex64 = /^[0-9a-f]{64}$/;
const route = /^\/v1\/checkpoints(?:\/([0-9a-f]{64})\/([0-9a-f]{64}))?$/;
const insertSQL = `INSERT INTO tronix_checkpoints
(artifact_sha,grant_sha,seq,chain_head,payload,payload_sha256)
SELECT ?,?,?,?,?,? WHERE NOT EXISTS
(SELECT 1 FROM tronix_checkpoints WHERE artifact_sha=? AND grant_sha=? AND seq>=?)`;
const latestSQL = `SELECT seq,payload,payload_sha256 FROM tronix_checkpoints
WHERE artifact_sha=? AND grant_sha=? ORDER BY seq DESC LIMIT 1`;

const json=(data,status=200,headers={})=>new Response(JSON.stringify(data),
  {status,headers:{'content-type':'application/json','cache-control':'no-store',...headers}});
const bytes=hex=>Uint8Array.from(hex.match(/../g),b=>parseInt(b,16));
const hex=data=>[...new Uint8Array(data)].map(x=>x.toString(16).padStart(2,'0')).join('');
async function sha(data){return hex(await crypto.subtle.digest('SHA-256',utf8.encode(data)));}
function equal(a,b){
  if(typeof a!=='string'||typeof b!=='string'||a.length>256||b.length>256)return false;
  const x=utf8.encode(a),y=utf8.encode(b);let diff=x.length^y.length;
  for(let i=0;i<Math.max(x.length,y.length);i++)diff|=(x[i]??0)^(y[i]??0);
  return diff===0;
}
function configured(env){
  return env.DB && hex64.test(env.CHECKPOINT_HMAC_KEY_HEX??'') &&
    typeof env.PUBLISH_TOKEN==='string' && env.PUBLISH_TOKEN.length>=32 &&
    typeof env.READ_TOKEN==='string' && env.READ_TOKEN.length>=32 &&
    !equal(env.PUBLISH_TOKEN,env.READ_TOKEN);
}
function parse(raw){
  if(raw.length<1||raw.length>2048||!raw.endsWith('\n')||
     /[^\x20-\x7e\n]/.test(raw))return null;
  let obj;
  try{obj=JSON.parse(raw);}catch{return null;}
  if(!obj||typeof obj!=='object'||Array.isArray(obj)||
     Object.keys(obj).length!==6)return null;
  const {artifactSha256,chainHead,grantSha256,schema,sequence,signature}=obj;
  if(schema!=='tronix.evidence-checkpoint.v1'||
     !Number.isSafeInteger(sequence)||sequence<1||
     ![artifactSha256,chainHead,grantSha256,signature].every(
       v=>typeof v==='string'&&hex64.test(v)))return null;
  const canonical=JSON.stringify({artifactSha256,chainHead,grantSha256,
    schema,sequence,signature})+'\n';
  return raw===canonical?obj:null;
}
async function checkSignature(item,secret){
  const {artifactSha256,chainHead,grantSha256,schema,sequence,signature}=item;
  const body=JSON.stringify({artifactSha256,chainHead,grantSha256,schema,sequence});
  const key=await crypto.subtle.importKey('raw',bytes(secret),
    {name:'HMAC',hash:'SHA-256'},false,['verify']);
  return crypto.subtle.verify('HMAC',key,bytes(signature),utf8.encode(body));
}
function auth(req,token){return equal(req.headers.get('authorization'),'Bearer '+token);}
async function boundedBody(request, cap=2048){
  const reader=request.body?.getReader();
  if(!reader)return {error:'INVALID_BODY'};
  let total=0,parts=[];
  try{
    while(true){
      const {done,value}=await reader.read();
      if(done)break;
      total+=value.byteLength;
      if(total>cap){
        await reader.cancel();
        return {error:'TOO_LARGE'};
      }
      parts.push(value);
    }
    const joined=new Uint8Array(total);
    let index=0;
    for(const part of parts){joined.set(part,index);index+=part.byteLength;}
    return {text:new TextDecoder('utf-8',{fatal:true}).decode(joined)};
  }catch{
    return {error:'INVALID_BODY'};
  }finally{try{reader.releaseLock();}catch{}}
}
export default {
  async fetch(request,env){
    if(!configured(env))return json({error:'NOT_CONFIGURED'},503);
    const url=new URL(request.url);
    const match=route.exec(url.pathname);
    if(!match)return json({error:'NOT_FOUND'},404);
    if(request.method==='POST'&&!match[1]){
      if(!auth(request,env.PUBLISH_TOKEN))return json({error:'UNAUTHORIZED'},401);
      if(request.headers.get('content-type')?.split(';')[0].trim()!=='application/json')
        return json({error:'CONTENT_TYPE_REQUIRED'},415);
      if(Number(request.headers.get('content-length')||0)>2048)
        return json({error:'TOO_LARGE'},413);
      const bounded=await boundedBody(request);
      if(bounded.error==='TOO_LARGE')return json({error:'TOO_LARGE'},413);
      if(bounded.error)return json({error:'INVALID_BODY'},400);
      const raw=bounded.text;
      const item=parse(raw);
      if(!item)return json({error:'INVALID_CHECKPOINT'},400);
      if(!await checkSignature(item,env.CHECKPOINT_HMAC_KEY_HEX))
        return json({error:'INVALID_SIGNATURE'},403);
      const checksum=await sha(raw);
      try{
        const result=await env.DB.prepare(insertSQL).bind(
          item.artifactSha256,item.grantSha256,item.sequence,item.chainHead,
          raw,checksum,item.artifactSha256,item.grantSha256,item.sequence).run();
        if(result?.meta?.changes===1)
          return json({state:'published',sequence:item.sequence,sha256:checksum},201);
        const existing=await env.DB.prepare(latestSQL)
          .bind(item.artifactSha256,item.grantSha256).first();
        if(existing?.seq===item.sequence && existing.payload===raw &&
           existing.payload_sha256===checksum)
          return json({state:'already-published',sequence:item.sequence,sha256:checksum},200);
        return json({error:'STALE_OR_CONFLICTING_CHECKPOINT'},409);
      }catch{return json({error:'STORE_UNAVAILABLE'},503);}
    }
    if(request.method==='GET'&&match[1]){
      if(!auth(request,env.READ_TOKEN))return json({error:'UNAUTHORIZED'},401);
      const keys=[...url.searchParams.keys()];
      if(keys.some(k=>k!=='sequence') || keys.length>1)
        return json({error:'INVALID_QUERY'},400);
      const at=url.searchParams.get('sequence');
      if(at!==null && (!/^[1-9][0-9]*$/.test(at) ||
                       !Number.isSafeInteger(Number(at))))
        return json({error:'INVALID_SEQUENCE'},400);
      try{
        const lookup=at===null
          ?env.DB.prepare(latestSQL).bind(match[1],match[2])
          :env.DB.prepare(`SELECT seq,payload,payload_sha256 FROM tronix_checkpoints
WHERE artifact_sha=? AND grant_sha=? AND seq=?`).bind(match[1],match[2],Number(at));
        const found=await lookup.first();
        if(!found)return json({error:'NO_CHECKPOINT'},404);
        return new Response(found.payload,{status:200,
          headers:{'content-type':'application/json','cache-control':'no-store',
            'x-tronix-checkpoint-sha256':found.payload_sha256}});
      }catch{return json({error:'STORE_UNAVAILABLE'},503);}
    }
    return json({error:'METHOD_NOT_ALLOWED'},405,{allow:'GET, POST'});
  }
};
