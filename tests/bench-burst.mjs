// Reproducible fixture-level scheduler benchmark; not a real-time certification.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';

const root=resolve(import.meta.dirname,'..');
const dir=await mkdtemp(join(tmpdir(),'tronix-burst-'));
const proc=spawn(join(root,'build/tronixd'),[
  '--socket',join(dir,'runtime.sock'),
  '--scheduler',join(root,'build/tronix_scheduler.zip'),
  '--adapter',join(root,'build/tronix_demo.zip')
],{cwd:root,stdio:['ignore','pipe','pipe']});
let stdout='',stderr='';
proc.stdout.on('data',b=>{stdout+=b.toString();});
proc.stderr.on('data',b=>{stderr+=b.toString();});
const client=new TronixClient(join(dir,'runtime.sock'));
const percentile=(values,p)=>{
  const v=[...values].sort((a,b)=>a-b);
  return v[Math.ceil(p*v.length)-1];
};
async function finished(id){
  for(let tries=0;tries<800;tries++){
    const item=await client.result(id);
    if(['completed','failed','expired','overrun'].includes(item.state))return item;
    await delay(5);
  }
  throw Error('No terminal result: '+id);
}
try{
  for(let i=0;i<250&&!stdout.includes('TRONIX_READY');i++){
    if(proc.exitCode!==null)throw Error('daemon startup failed: '+stderr);
    await delay(10);
  }
  assert.ok(stdout.includes('TRONIX_READY'));
  assert.ok((await client.submit('block','fixture','demo.slow',5000)).ok);
  for(let i=0;i<100;i++){
    if((await client.result('block')).state==='running')break;
    await delay(2);
  }
  assert.equal((await client.result('block')).state,'running');
  const ids=Array.from({length:64},(_,i)=>'burst-'+i);
  for(let i=0;i<ids.length;i+=8){
    const response=await Promise.all(ids.slice(i,i+8).map((id,n)=>
      client.submit(id,'agent-'+((i+n)%8),'demo.constant',5000)));
    for(const r of response)assert.equal(r.ok,true,JSON.stringify(r));
  }
  const rows=await Promise.all(ids.map(finished));
  for(let i=0;i<rows.length;i++){
    const r=rows[i];
    assert.equal(r.state,'completed',ids[i]);
    assert.equal(r.value,42);
    assert.equal(r.budgetMs,1000);
    assert.ok(r.queueWaitNs>=0 && r.executionNs>0);
  }
  assert.equal((await finished('block')).state,'completed');
  const snap=(await client.snapshot()).stats;
  assert.equal(snap.dispatched,65);
  assert.equal(snap.completed,65);
  assert.equal(snap.overrun,0);
  assert.equal(snap.active,0);
  assert.equal(snap.pending,0);
  assert.ok(snap.maxQueueWaitNs>=Math.max(...rows.map(x=>x.queueWaitNs)));
  assert.ok(snap.maxExecutionNs>=Math.max(...rows.map(x=>x.executionNs)));
  for(const id of [...ids,'block']) assert.equal((await client.ack(id)).ok,true);
  const ms=x=>Math.round(x/1e3)/1e3;
  console.log(JSON.stringify({
    schema:'tronix.burst.v1',outcome:'PASS',workload:{
      operations:rows.length,agents:8,blockingOperationMs:150,lane:'interactive'
    },queueWaitMs:{
      p50:ms(percentile(rows.map(x=>x.queueWaitNs),.50)),
      p95:ms(percentile(rows.map(x=>x.queueWaitNs),.95)),
      max:ms(snap.maxQueueWaitNs)
    },executionMs:{
      p50:ms(percentile(rows.map(x=>x.executionNs),.50)),
      p95:ms(percentile(rows.map(x=>x.executionNs),.95))
    },counters:snap
  },null,2));
}catch(error){
  console.error(JSON.stringify({schema:'tronix.burst.v1',outcome:'FAIL',error:String(error),stderr},null,2));
  process.exitCode=1;
}finally{
  const ended=new Promise(r=>proc.once('close',r));
  if(proc.exitCode===null)proc.kill('SIGTERM');
  await Promise.race([ended,delay(1500)]);
  if(proc.exitCode===null)proc.kill('SIGKILL');
  await rm(dir,{recursive:true,force:true});
}
