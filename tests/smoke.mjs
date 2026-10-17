import assert from 'node:assert/strict';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {mkdtemp,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {resolve,join} from 'node:path';
import {encode,decode} from '@msgpack/msgpack';
import {TronixClient} from '../src/client.mjs';
import {setTimeout as delay} from 'node:timers/promises';
const dir=await mkdtemp(join(tmpdir(),'tronix-test-'));
const socket=join(dir,'runtime.sock');
const root=resolve(import.meta.dirname,'..');
const child=spawn(join(root,'build/tronixd'),[
  '--socket',socket,'--scheduler',join(root,'build/tronix_scheduler.zip'),
  '--adapter',join(root,'build/tronix_demo.zip')],{cwd:root,stdio:['ignore','pipe','pipe']});
let stderr='',stdout='';
child.stderr.on('data',b=>{stderr+=b.toString();});
child.stdout.on('data',b=>{stdout+=b.toString();});
async function ready(){
  for(let i=0;i<150;i++){
    if(stdout.includes('TRONIX_READY'))return;
    if(child.exitCode!==null)throw Error('daemon exited '+child.exitCode+stderr);
    await delay(20);
  }
  throw Error('daemon not ready '+stderr);
}
function call(payload){
  return new Promise((resolvePromise,reject)=>{
    const s=net.createConnection(socket);
    const timer=setTimeout(()=>{s.destroy();reject(Error('socket request timeout'));},3000);
    let buf=Buffer.alloc(0);
    const finish=(err,result)=>{clearTimeout(timer);s.destroy();if(err)reject(err);else resolvePromise(result);};
    s.on('error',err=>finish(err));
    s.on('close',()=>{if(buf.length===0)finish(Error('connection closed without response'));});
    s.on('data',chunk=>{
      buf=Buffer.concat([buf,chunk]);
      if(buf.length>=4){
        const size=buf.readUInt32BE(0);
        if(buf.length>=size+4){try{finish(null,decode(buf.subarray(4,size+4)));}catch(err){finish(err);}}
      }
    });
    s.on('connect',()=>{
      const b=Buffer.from(encode(payload)),header=Buffer.alloc(4);
      header.writeUInt32BE(b.length);s.write(Buffer.concat([header,b]));
    });
  });
}
const one=(action,fields={})=>call({v:1,action,...fields});
const submit=(id,agent,operation,deadlineMs=5000)=>one('submit',{id,agent,operation,deadlineMs});
const result=id=>one('result',{id});
const ack=id=>one('ack',{id});
async function until(id,terminal=['completed','failed','expired']){
  for(let i=0;i<400;i++){const s=await result(id);if(terminal.includes(s.state))return s;await delay(5);}
  throw Error('job did not finish: '+id);
}
const report={schema:'tronix.smoke.v1',checks:[],snapshots:{}};
const check=(name,condition)=>{assert.ok(condition,name);report.checks.push(name);};
try{
  await ready();
  const ping=await one('ping');
  check('native daemon starts and replies via MessagePack',ping.ok&&ping.runtime==='tronix/0.1');
  const typedClient=new TronixClient(socket);
  check('published Node IPC client works', (await typedClient.ping()).runtime==='tronix/0.1');
  const caps=await one('capabilities');
  check('Celix adapter capabilities discovered',caps.ok&&caps.service==='org.tronix.operation'&&caps.operations.includes('demo.constant'));
  const rejected=await submit('bad-1','attacker','Runtime.evaluate');
  check('raw browser commands denied',rejected.error==='DENIED');
  const invalid=await call({v:1,action:'submit',id:'bad-2',agent:'user',
    operation:'demo.constant',deadlineMs:1000,lane:'interactive'});
  check('untrusted priority fails closed',invalid.error==='BAD_REQUEST');
  const accepted=await submit('a-1','a','demo.constant');
  const duplicate=await submit('a-1','a','demo.constant');
  check('duplicate ID rejected',accepted.ok && duplicate.error==='DUPLICATE_ID');
  const done=await until('a-1');
  check('Celix demo bundle executes and returns 42',done.ok&&done.value===42&&done.dispatchSequence>0);
  check('ack releases terminal results', (await ack('a-1')).ok && (await result('a-1')).error==='NOT_FOUND');
  const blocker=await submit('block','blocker','demo.pause');
  check('background blocker submitted',blocker.ok);
  // Wait for operation to run, ensuring priority controls queued work rather than preemption.
  for(let i=0;i<50;i++){if((await result('block')).state==='running')break;await delay(1);}
  const background=await submit('back','background','demo.pause');
  const interactive=await submit('urgent','interactive','demo.constant');
  check('mixed priority work admitted',background.ok&&interactive.ok);
  const [rBlock,rBack,rUrgent]=await Promise.all([until('block'),until('back'),until('urgent')]);
  check('priority overtakes queued background after active job',rUrgent.dispatchSequence<rBack.dispatchSequence);
  check('all admitted jobs complete', [rBlock,rBack,rUrgent].every(x=>x.state==='completed'));
  for(const id of ['block','back','urgent'])await ack(id);
  await submit('hold','hold','demo.pause');
  for(let i=0;i<50;i++){if((await result('hold')).state==='running')break;await delay(1);}
  await submit('deadline','late','demo.pause',1);
  const expired=await until('deadline');
  check('monotonic queue deadline expires without execution',expired.state==='expired'&&expired.dispatchSequence===0);
  await until('hold');await ack('hold');await ack('deadline');
  const statuses=[];
  for(let i=0;i<137;i++){
    const id='load-'+i;
    statuses.push({id,...(await submit(id,'agent-'+(i%6),'demo.pause',30000))});
  }
  const rejectedOverload=statuses.filter(x=>x.error==='QUEUE_FULL');
  check('queue admission bounded under overload',rejectedOverload.length>0);
  const snap=(await one('snapshot')).stats;
  report.snapshots.peak=snap;
  check('scheduler metrics expose bounded pending and single active task',
    snap.peakPending<=128 && snap.active<=1 && snap.rejected>=1 && snap.dispatched>=3);
  const acceptedLoad=statuses.filter(x=>x.ok);
  // Drain and acknowledge all admitted jobs before exit.
  for(const job of acceptedLoad){await until(job.id);await ack(job.id);}
  // Reuse agent accounting for more identities than the fixed internal slot count.
  for(let i=0;i<300;i++){
    const id='churn-'+i;
    assert.equal((await submit(id,'identity-'+i,'demo.constant')).ok,true);
    assert.equal((await until(id)).value,42);
    assert.equal((await ack(id)).ok,true);
  }
  check('300 rotating agents do not exhaust native identity slots',true);
  const final=(await one('snapshot')).stats;
  report.snapshots.final=final;
  check('all accepted load work finished',final.pending===0 && final.active===0);
  report.outcome='PASS';
  console.log(JSON.stringify(report,null,2));
}finally{
  child.kill('SIGTERM');
  await Promise.race([new Promise(r=>child.once('exit',r)),delay(3000)]);
  if(child.exitCode===null)child.kill('SIGKILL');
  await rm(dir,{recursive:true,force:true});
}
