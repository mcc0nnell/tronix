// SPDX-License-Identifier: Apache-2.0
// Local fixture benchmark with rotating order. Never an end-to-end cost claim.
import {spawn} from 'node:child_process';
import {mkdtemp,mkdir,writeFile,readFile,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import path from 'node:path';
import {createHash} from 'node:crypto';
import {performance} from 'node:perf_hooks';
import {verifyBatch} from '../src/verify_batch.mjs';
import {verifyRequest} from '../src/verify_client.mjs';

const root=path.resolve(import.meta.dirname,'..');
const build=process.env.TRONIX_BUILD||path.join(root,'build');
const daemon=path.join(build,'tronix_verify_daemon');
const bundle=path.join(build,'tronix_answer_verifier.zip');
const hash=b=>createHash('sha256').update(b).digest('hex');
const payload=Buffer.from('42\n');
const work=await mkdtemp(path.join(tmpdir(),'tronix-pool-bench-'));
const bundleHash=hash(await readFile(bundle));
const pct=(sorted,p)=>sorted[Math.ceil(sorted.length*p)-1];
const count=Number(process.env.TRONIX_BENCH_COUNT||800);
const rounds=Number(process.env.TRONIX_BENCH_ROUNDS||4);
const configurations=(process.env.TRONIX_BENCH_WORKERS||'1,2,4,8').split(',').map(Number);
if(!Number.isInteger(count)||count<100||count>5000)throw Error('count must be 100..5000');
if(!Number.isInteger(rounds)||rounds<1||rounds>10)throw Error('rounds must be 1..10');
if(!configurations.length||configurations.some(n=>![1,2,4,8].includes(n))||
   new Set(configurations).size!==configurations.length)
  throw Error('workers must be a unique list from 1,2,4,8');

async function start(socket,oracle,workers){
  const child=spawn(daemon,['--socket',socket,'--oracle',oracle,
    '--bundle',bundle,'--bundle-sha256',bundleHash,'--workers',String(workers)],
    {stdio:['ignore','pipe','pipe'],env:{PATH:process.env.PATH}});
  let stdout='',stderr='';
  child.stdout.on('data',b=>stdout+=b.toString());
  child.stderr.on('data',b=>stderr+=b.toString());
  await new Promise((resolve,reject)=>{
    let done=false;
    const finish=e=>{if(done)return;done=true;clearTimeout(timer);if(e)reject(e);else resolve();};
    const timer=setTimeout(()=>finish(Error('daemon startup timeout '+stderr)),7000);
    child.stdout.on('data',()=>{if(stdout.includes('TRONIX_VERIFY_READY'))finish();});
    child.once('exit',()=>finish(Error('daemon exited '+stderr)));
  });
  return child;
}
async function batch(socket,n,samples){
  const items=Array.from({length:n},(_,i)=>({
    candidateId:'c'+i,taskId:'golden',answer:payload
  }));
  const result=await verifyBatch(items,async(taskId,answer)=>{
    const start=performance.now();
    const r=await verifyRequest(socket,taskId,answer);
    if(samples)samples.push(performance.now()-start);
    return r;
  },{concurrency:8});
  if(result.some(r=>r.status!=='VERIFIED'))throw Error('unexpected native decision');
}
async function workload(socket,n,samples){
  for(let remaining=n;remaining>0;remaining-=Math.min(remaining,100)){
    await batch(socket,Math.min(remaining,100),samples);
  }
}
const daemons=[];
try{
  const oracle=path.join(work,'oracle');
  await writeFile(oracle,'golden '+hash(payload)+'\n',{mode:0o600});
  for(const workers of configurations){
    const folder=path.join(work,'pool'+workers);
    await mkdir(folder,{mode:0o700});
    const socket=path.join(folder,'local.sock');
    const child=await start(socket,oracle,workers);
    daemons.push({child,workers,socket});
  }
  // Warm every running configuration BEFORE timed rounds; avoid cold-start bias.
  for(const d of daemons)await workload(d.socket,400);
  const results=new Map(configurations.map(w=>[w,[]]));
  for(let round=0;round<rounds;round++){
    const order=round%2===0?daemons:[...daemons].reverse();
    for(const d of order){
      const samples=[];
      const start=performance.now();
      await workload(d.socket,count,samples);
      const elapsed=performance.now()-start;
      samples.sort((a,b)=>a-b);
      const row={
        schema:'tronix.native-pool.fixture-benchmark/v2',
        round:round+1,workers:d.workers,requests:count,inFlight:8,
        elapsedMs:Number(elapsed.toFixed(2)),
        requestsPerSecond:Number((count*1000/elapsed).toFixed(1)),
        p50Ms:Number(pct(samples,.5).toFixed(3)),
        p95Ms:Number(pct(samples,.95).toFixed(3)),
        p99Ms:Number(pct(samples,.99).toFixed(3))
      };
      results.get(d.workers).push(row);
      console.log(JSON.stringify(row));
    }
  }
  for(const w of configurations){
    const rps=results.get(w).map(r=>r.requestsPerSecond).sort((a,b)=>a-b);
    const mean=rps.reduce((a,b)=>a+b,0)/rps.length;
    console.log(JSON.stringify({
      schema:'tronix.native-pool.fixture-benchmark-summary/v2',
      workers:w,rounds,requestsPerRound:count,concurrency:8,
      meanRps:Number(mean.toFixed(1)),minRps:rps[0],maxRps:rps.at(-1),
      caveat:'Local Node IPC+Celix+SHA fixture; host noise and JIT may dominate. No LLM, WASM, corpus or Rizzer pipeline.'
    }));
  }
}finally{
  for(const {child} of daemons){
    if(child.exitCode===null){
      const exited=new Promise(resolve=>child.once('exit',resolve));
      child.kill('SIGTERM');
      await exited;
    }
  }
  await rm(work,{recursive:true,force:true});
}
