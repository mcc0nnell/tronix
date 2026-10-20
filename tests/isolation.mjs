import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,rm,readdir,writeFile,chmod} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';

const root=resolve(import.meta.dirname,'..');
const dir=await mkdtemp(join(tmpdir(),'tronix-worker-test-'));
const socket=join(dir,'runtime.sock'),journal=join(dir,'journal.msgpack');
const policy=join(dir,'policy.conf');
const principal='uid:'+process.getuid();
await writeFile(policy,'uid '+process.getuid()+'\n'+
  ['demo.constant','demo.hang','demo.crash','demo.pid'].map(op=>'allow '+op).join('\n')+'\n',{mode:0o600});
await chmod(policy,0o600);
const oldCaches=new Set((await readdir(tmpdir())).filter(n=>n.startsWith('tronix-worker-')));
const daemon=spawn(join(root,'build/tronixd'),[
  '--socket',socket,'--scheduler',join(root,'build/tronix_scheduler.zip'),
  '--adapter',join(root,'build/tronix_demo.zip'),'--journal',journal,
  '--worker',join(root,'build/tronix_worker'),'--policy',policy
],{cwd:root,env:{...process.env,TRONIX_ISOLATION_FIXTURES:'1'},
   stdio:['ignore','pipe','pipe']});
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
  const response=await client.submit(id,principal,operation,2000);
  check(operation+' admitted',response.ok);
  return await until(id);
}
try{
  for(let i=0;i<200&&!out.includes('TRONIX_READY');i++){
    if(daemon.exitCode!==null)throw Error('daemon exited: '+err);
    await delay(10);
  }
  check('native supervised daemon starts',out.includes('TRONIX_READY'));
  const cap=await client.capabilities();
  check('trusted demo fixture permits isolation checks',
        cap.operations.includes('demo.hang')&&cap.operations.includes('demo.crash'));
  check('raw command remains denied',
        (await client.submit('denied',principal,'bash')).error==='DENIED');
  const pid=await execute('pid-1','demo.pid');
  check('Celix tool executed outside daemon PID',pid.state==='completed'&&
        pid.value!==daemon.pid&&pid.value>0);
  check('finished worker was reaped',!(() => {try{process.kill(pid.value,0);return true;}catch{return false;}})());
  await client.ack('pid-1');
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
  check('ledger records actual overrun',(await client.request('receipt',{id:'hung'})).receipt.state==='overrun');
  await client.ack('hung');
  const after=await execute('after','demo.constant');
  check('post-failure worker starts and completes',after.state==='completed'&&after.value===42);
  await client.ack('after');
  const events=await client.request('events',{after:0,limit:16});
  check('isolated runs retain sequenced events',events.events.some(e=>e.state==='failed') &&
        events.events.some(e=>e.state==='overrun'));
  const created=(await readdir(tmpdir())).filter(n=>
      n.startsWith('tronix-worker-')&&!oldCaches.has(n));
  check('worker cache cleaned even after SIGKILL',created.length===0);
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
