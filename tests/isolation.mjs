import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,rm,readdir,writeFile,readFile,unlink,readlink} from 'node:fs/promises';
import {openSync,closeSync} from 'node:fs';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';

const root=resolve(import.meta.dirname,'..');
const dir=await mkdtemp(join(tmpdir(),'tronix-worker-test-'));
const socket=join(dir,'runtime.sock'),journal=join(dir,'journal.msgpack');
const memory=join(dir,'memory.msgpack');
const sentinel=join(dir,'tronix-fd-canary-demo.txt');
await writeFile(sentinel,'synthetic nonsecret descriptor canary\n');
const sentinelFD=openSync(sentinel,'r');

const activatorMarker=join(dir,'activator.pids');
const driftFile=join(dir,'policy-drift.flag');
await writeFile(activatorMarker,'',{mode:0o600});
const oldCaches=new Set((await readdir(tmpdir())).filter(n=>n.startsWith('tronix-worker-')));
const daemon=spawn(join(root,'build/tronixd'),[
  '--socket',socket,'--scheduler',join(root,'build/tronix_scheduler.zip'),
  '--adapter',join(root,'build/tronix_demo.zip'),'--journal',journal,
  '--worker',join(root,'build/tronix_worker'),'--memory',memory
],{cwd:root,env:{...process.env,TRONIX_ISOLATION_FIXTURES:'1',
            TRONIX_ACTIVATOR_MARKER:activatorMarker,
            TRONIX_POLICY_DRIFT_FILE:driftFile,
            TRONIX_SECRET_CANARY:'synthetic-secret-not-real'},
   stdio:['ignore','pipe','pipe','ignore','ignore',sentinelFD]});
closeSync(sentinelFD);
let out='',err='';
daemon.stdout.on('data',b=>{out+=b.toString();});
daemon.stderr.on('data',b=>{err+=b.toString();});
const client=new TronixClient(socket),checks=[];
const check=(name,condition)=>{assert.ok(condition,name);checks.push(name);};
async function until(id){
  for(let i=0;i<300;i++){
    const r=await client.result(id);
    if(['completed','failed','expired','overrun'].includes(r.state))return r;
    await delay(10);
  }
  throw Error('timeout waiting for '+id);
}
async function execute(id,operation){
  const response=await client.submit(id,'trusted-agent',operation,2000);
  check(operation+' admitted',response.ok);
  return await until(id);
}
try{
  for(let i=0;i<200&&!out.includes('TRONIX_READY');i++){
    if(daemon.exitCode!==null)throw Error('daemon exited: '+err);
    await delay(10);
  }
  check('native supervised daemon starts',out.includes('TRONIX_READY'));
  check('daemon retains synthetic inheritable descriptor',
        (await readlink('/proc/'+daemon.pid+'/fd/5')).includes('tronix-fd-canary-'));

  const observedPids=(await readFile(activatorMarker,'utf8')).trim().split('\n').map(Number);
  check('adapter activation ran only in inspected child, never main daemon',
        observedPids.length>=1 && observedPids.every(pid=>pid>0&&pid!==daemon.pid));
  const cap=await client.capabilities();
  check('trusted demo fixture permits isolation checks',
        cap.operations.includes('demo.hang')&&cap.operations.includes('demo.crash'));
  check('memory coexists with journal and supervised worker',
        (await client.memoryPut('trusted-agent','hello','world')).ok &&
        (await client.memoryGet('trusted-agent','hello')).value==='world');
  check('raw command remains denied',
        (await client.submit('denied','trusted-agent','bash')).error==='DENIED');
  const pid=await execute('pid-1','demo.pid');
  check('Celix tool executed outside daemon PID',pid.state==='completed'&&
        pid.value!==daemon.pid&&pid.value>0);
  check('finished worker was reaped',!(() => {try{process.kill(pid.value,0);return true;}catch{return false;}})());
  await client.ack('pid-1');
  for(const [op,title] of [
    ['demo.env_clean','synthetic environment secret stripped'],
    ['demo.fd_clean','synthetic inherited descriptor stripped'],
    ['demo.no_new_privs','Linux NO_NEW_PRIVS enforced']
  ]){
    const result=await execute(op+'-id',op);
    check(title,result.state==='completed'&&result.value===1);
    await client.ack(op+'-id');
  }
  const quick=await execute('quick','demo.constant');
  check('approved child result returned',quick.state==='completed'&&quick.value===42);
  await client.ack('quick');
  const crash=await execute('crash','demo.crash');
  check('child process abort reported as failed',crash.state==='failed');
  check('daemon survives adapter abort',(await client.ping()).ok);
  await client.ack('crash');
  const began=performance.now();
  const hung=await execute('hung','demo.hang');
  const elapsed=performance.now()-began;
  check('hung operation terminated by supervisor',hung.state==='overrun'&&elapsed<2500);
  check('overrun never exposes result',!Object.hasOwn(hung,'value'));
  check('ledger records actual overrun',(await client.receipt('hung')).receipt.state==='overrun');
  await client.ack('hung');
  await writeFile(driftFile,'drift',{mode:0o600});
  const policyDrift=await execute('drift','demo.constant');
  check('child denies policy drift from frozen admission snapshot',
        policyDrift.state==='failed' && !Object.hasOwn(policyDrift,'value'));
  await client.ack('drift');
  await unlink(driftFile);
  const after=await execute('after','demo.constant');
  check('post-failure worker starts and completes',after.state==='completed'&&after.value===42);
  await client.ack('after');
  let cursor=0,events=[];
  for(let page=0;page<8;page++){
    const batch=await client.events(cursor,16);
    events.push(...batch.events);
    if(!batch.events.length || batch.lastSeq===batch.events.at(-1).seq)break;
    cursor=batch.events.at(-1).seq;
  }
  check('isolated runs retain sequenced events',events.some(e=>e.state==='failed') &&
        events.some(e=>e.state==='overrun') &&
        events.every((e,i)=>i===0 || e.seq===events[i-1].seq+1));
  // Invalid/unloadable adapter must never allow daemon startup.
  const bad=spawn(join(root,'build/tronixd'),[
    '--socket',join(dir,'invalid.sock'),
    '--scheduler',join(root,'build/tronix_scheduler.zip'),
    '--adapter',join(dir,'no-such-bundle.zip'),
    '--worker',join(root,'build/tronix_worker')
  ],{cwd:root,stdio:['ignore','pipe','pipe']});
  let badOut='',badErr='';
  bad.stdout.on('data',b=>badOut+=b.toString());
  bad.stderr.on('data',b=>badErr+=b.toString());
  const failedStartup=await Promise.race([
    new Promise(resolve=>bad.once('close',resolve)),
    delay(5000).then(()=>null)
  ]);
  if(failedStartup===null)bad.kill('SIGKILL');
  check('supervised policy inspection refuses unloadable adapters',
        failedStartup!==null&&failedStartup!==0 &&
        !badOut.includes('TRONIX_READY') &&
        badErr.includes('policy inspection failed'));
  const created=(await readdir(tmpdir())).filter(n=>
      n.startsWith('tronix-worker-')&&!oldCaches.has(n));
  check('worker cache cleaned even after SIGKILL',created.length===0);
  const allPids=(await readFile(activatorMarker,'utf8')).trim().split('\n').map(Number);
  check('policy and execution adapter activation remain out-of-process',
        allPids.length>=5 && allPids.every(pid=>pid!==daemon.pid));
  console.log(JSON.stringify({schema:'tronix.supervision.v1',outcome:'PASS',checks,
    observedTimeoutMs:elapsed},null,2));
}catch(e){
  console.error(JSON.stringify({schema:'tronix.supervision.v1',outcome:'FAIL',checks,
    error:String(e),stderr:err.slice(-2000)},null,2));
  process.exitCode=1;
}finally{
  const closed=new Promise(resolve=>daemon.once('close',resolve));
  daemon.kill('SIGTERM');
  const code=await Promise.race([closed,delay(2000).then(()=>null)]);
  if(code===null){daemon.kill('SIGKILL');await closed;}
  await rm(dir,{recursive:true,force:true});
}
