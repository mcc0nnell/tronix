// SPDX-License-Identifier: Apache-2.0
// Native-backed pool regression; never sends model or arbitrary execution commands.
import assert from 'node:assert/strict';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {mkdtemp,mkdir,writeFile,rm,stat} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import path from 'node:path';
import {once} from 'node:events';
import {setTimeout as delay} from 'node:timers/promises';
import {verifyRequest,verifyStats} from '../src/verify_client.mjs';
import {verifyBatch} from '../src/verify_batch.mjs';

const root=path.resolve(import.meta.dirname,'..');
const build=process.env.TRONIX_BUILD||path.join(root,'build');
const exe=path.join(build,'tronix_verify_daemon');
const bundle=path.join(build,'tronix_answer_verifier.zip');
const digest=buffer=>createHash('sha256').update(buffer).digest('hex');
const fs=await import('node:fs/promises');
const bundleHash=digest(await fs.readFile(bundle));
const correct=Buffer.from('42\n');
const oracleText='golden '+digest(correct)+'\n';
const dir=await mkdtemp(path.join(tmpdir(),'tronix-worker-pool-'));
const privateDir=path.join(dir,'private');
const oracle=path.join(dir,'oracles');
const socket=path.join(privateDir,'verify.sock');
await mkdir(privateDir,{mode:0o700});
await writeFile(oracle,oracleText,{mode:0o600});
const baseArgs=['--socket',socket,'--oracle',oracle,'--bundle',bundle,
  '--bundle-sha256',bundleHash];

function launch(additional,readyExpected=true){
  const child=spawn(exe,[...baseArgs,...additional],{
    stdio:['ignore','pipe','pipe'],env:{PATH:process.env.PATH}
  });
  let stdout='',stderr='';
  child.stdout.on('data',buffer=>{stdout+=buffer.toString();});
  child.stderr.on('data',buffer=>{stderr+=buffer.toString();});
  const ready=new Promise((resolve,reject)=>{
    let done=false;
    const finish=(error,value)=>{
      if(done)return;done=true;clearTimeout(timer);
      if(error)reject(error);else resolve(value);
    };
    const timer=setTimeout(()=>finish(Error('startup timed out: '+stderr)),6000);
    child.stdout.on('data',()=>{
      if(stdout.includes('TRONIX_VERIFY_READY'))finish(null,true);
    });
    child.once('exit',()=>finish(null,false));
  });
  return {child,ready:ready.then(value=>{
    if(readyExpected&&!value)throw Error('startup failed: '+stderr);
    return value;
  }),stderr:()=>stderr};
}
async function stop(child){
  if(child.exitCode!==null)return;
  const exited=once(child,'exit');
  child.kill('SIGTERM');
  await Promise.race([exited,delay(6000).then(()=>{throw Error('shutdown timeout');})]);
}
let passed=0;
async function check(name,fn){
  await fn();
  passed++;
  console.log('PASS',name);
}
try{
  await check('invalid pool sizes fail closed before binding socket',async()=>{
    for(const value of ['0','9','01','-1','x']){
      const run=launch(['--workers',value],false);
      assert.equal(await run.ready,false);
      assert.equal(run.child.exitCode,3);
      await assert.rejects(stat(socket));
    }
  });
  await check('slow-drip peer is closed at absolute connection deadline',async()=>{
    const single=launch(['--workers','1']);
    assert.equal(await single.ready,true);
    let drip,interval;
    try{
      drip=net.createConnection(socket);
      await once(drip,'connect');
      drip.write(Buffer.from([0,0,0,100])); // 100-byte payload never completed
      const closed=new Promise(resolve=>drip.once('close',resolve));
      interval=setInterval(()=>{
        if(!drip.destroyed)drip.write(Buffer.from([0x01]));
      },100);
      await Promise.race([closed,delay(3500).then(()=>{
        throw Error('slow-drip peer exceeded absolute deadline');
      })]);
      const verified=await verifyRequest(socket,'golden',correct);
      assert.equal(verified.status,'VERIFIED');
    }finally{
      if(interval)clearInterval(interval);
      if(drip)drip.destroy();
      await stop(single.child);
    }
  });
  const server=launch(['--workers','4']);
  assert.equal(await server.ready,true);
  const slow=[];
  try{
    await check('three idle peers cannot starve fourth bounded native worker',async()=>{
      for(let i=0;i<3;i++){
        const peer=net.createConnection(socket);
        await once(peer,'connect');
        slow.push(peer);
        // Allow the native accept loop to assign each connection to a worker.
        await delay(45);
      }
      const result=await verifyRequest(socket,'golden',correct);
      assert.equal(result.status,'VERIFIED');
      assert.equal(result.executionAuthorized,false);
    });
    await check('batch requests across workers preserve native oracle decisions',async()=>{
      const candidates=Array.from({length:72},(_,i)=>({
        candidateId:'candidate_'+i,taskId:i%3===2?'unknown':'golden',
        answer:i%3===1?Buffer.from('wrong'):correct
      }));
      const results=await verifyBatch(candidates,
        (taskId,answer)=>verifyRequest(socket,taskId,answer),
        {concurrency:8});
      assert.equal(results.length,candidates.length);
      for(let i=0;i<results.length;i++){
        assert.equal(results[i].candidateId,'candidate_'+i);
        assert.equal(results[i].status,['VERIFIED','REJECTED','UNVERIFIABLE'][i%3]);
        assert.equal(results[i].receipt.objectiveTruthEstablished,false);
        assert.equal(results[i].receipt.published,false);
      }
      const stats=await verifyStats(socket);
      assert.equal(stats.requests,73);
      assert.equal(stats.verified,25);
      assert.equal(stats.rejected,24);
      assert.equal(stats.reviewRequired,24);
    });
    await check('multiple native clients do not change frozen oracle',async()=>{
      await writeFile(oracle,'golden '+digest(Buffer.from('wrong'))+'\n');
      const receipts=await Promise.all(Array.from({length:16},()=>verifyRequest(socket,'golden',correct)));
      for(const r of receipts){
        assert.equal(r.status,'VERIFIED');
        assert.equal(r.oracleManifestSha256,digest(Buffer.from(oracleText)));
      }
    });
  }finally{
    for(const peer of slow)peer.destroy();
    await stop(server.child);
  }
  await check('shutdown removes private socket',async()=>{
    await assert.rejects(stat(socket));
  });
  console.log('NATIVE_POOL_TESTS',passed,'passed');
}finally{
  await rm(dir,{recursive:true,force:true});
}
