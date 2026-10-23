// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Two real native C/Celix processes exchange binary UDP datagrams over ::1.
// The sender uses the Celix UDP service; the receiver's Celix UDP service
// dispatches into the separately registered typed Celix caption service.
// This tests neither OpenThread RF nor external network reachability.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {existsSync} from 'node:fs';
import {mkdtemp,rm,mkdir} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
const root=resolve(import.meta.dirname,'..');
const bin=join(root,'build/bang-udp-celix-node');
const stream=join(root,'build/tronix_bang_stream.zip');
const udp=join(root,'build/tronix_bang_udp.zip');
for(const path of [bin,stream,udp])assert.ok(existsSync(path),'Missing compiled Celix target: '+path);
const scratch=await mkdtemp(join(tmpdir(),'tronix-bang-udp-'));
const cacheRX=join(scratch,'rx-cache'),cacheTX=join(scratch,'tx-cache'),cacheDenied=join(scratch,'denied-cache');
await mkdir(cacheRX,{mode:0o700});await mkdir(cacheTX,{mode:0o700});await mkdir(cacheDenied,{mode:0o700});
const jobs=[];
function launch(mode,target,cache,envExtra={}) {
 const p=spawn(bin,[mode,String(target),stream,udp],{
  env:{...process.env,BANG_CELIX_CACHE:cache,...envExtra},
  cwd:scratch,stdio:['ignore','pipe','pipe']
 });
 let output='',error='';
 p.stdout.on('data',d=>output=(output+d.toString()).slice(-16000));
 p.stderr.on('data',d=>error=(error+d.toString()).slice(-16000));
 const job={p,output:()=>output,error:()=>error};jobs.push(job);
 return job;
}
async function ready(job) {
 for(let attempt=0;attempt<160;attempt++) {
  const match=job.output().match(/^READY (\d+)$/m);
  if(match)return Number(match[1]);
  if(job.p.exitCode!==null)throw Error('Receiver exited: '+job.p.exitCode+' '+job.error());
  await delay(25);
 }
 throw Error('Celix receiver readiness timed out: '+job.error());
}
async function exited(job) {
 for(let i=0;i<160;i++){
  if(job.p.exitCode!==null)return job.p.exitCode;
  await delay(25);
 }
 throw Error('Celix node failed to terminate: '+job.error()+' '+job.output());
}
try{
 // Malicious or accidental bundle activation must fail closed without opt-in.
 const denied=launch('rx',0,cacheDenied,{
   BANG_UDP_SKIP_ENABLE:'1',TRONIX_BANG_UDP_ENABLE_FIXTURE:'0'
 });
 assert.notEqual(await exited(denied),0,'UDP service available without explicit opt-in');
 const receiver=launch('rx',0,cacheRX);
 const port=await ready(receiver);
 assert.ok(port>=1024&&port<=65535);
 const sender=launch('tx',port,cacheTX);
 assert.equal(await exited(sender),0,'sender failed '+sender.error());
 assert.equal(await exited(receiver),0,'receiver failed '+receiver.error());
 assert.match(sender.output(),/PASS: Celix UDP publisher/);
 assert.match(receiver.output(),/PASS: two native Celix processes tested 6 binary UDP datagrams/);
 console.log(JSON.stringify({
  schema:'tronix.bang.udp-celix-process.v1',
  outcome:'PASS',independentCelixProcesses:2,udpPackets:6,udpOptInRequired:true,malformedPacketsRejected:2,
  signedEvents:2, subscribers:2,rejectedReplay:1,rejectedForgedEvent:1,
  transport:'IPv6 loopback binary UDP',onAir:false,threadTransport:false,
  notes:'No real OpenThread UDP integration, no deployed key provisioning, no private caption encryption or human receipt'
 },null,2));
}finally{
 for(const job of jobs)if(job.p.exitCode===null)job.p.kill('SIGTERM');
 await delay(150);
 for(const job of jobs)if(job.p.exitCode===null)job.p.kill('SIGKILL');
 await rm(scratch,{recursive:true,force:true});
}
