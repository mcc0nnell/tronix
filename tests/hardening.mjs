import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';

const root=resolve(import.meta.dirname,'..');
const dir=await mkdtemp(join(tmpdir(),'tronix-hardening-'));
const sock=join(dir,'daemon.sock');
const proc=spawn(join(root,'build/tronixd'),[
  '--socket',sock,
  '--scheduler',join(root,'build/tronix_scheduler.zip'),
  '--adapter',join(root,'build/tronix_demo.zip')
],{cwd:root,stdio:['ignore','pipe','pipe']});
let stdout='',stderr='';
proc.stdout.on('data',chunk=>{stdout+=chunk.toString();});
proc.stderr.on('data',chunk=>{stderr+=chunk.toString();});
const checks=[];
const check=(name,value)=>{assert.ok(value,name);checks.push(name);};
const client=new TronixClient(sock);
try {
  for(let i=0;i<200&&!stdout.includes('TRONIX_READY');i++) {
    if(proc.exitCode!==null)throw Error('daemon exited: '+stderr);
    await delay(10);
  }
  check('daemon ready',stdout.includes('TRONIX_READY'));
  check('slow fixture supported',(await client.capabilities()).operations.includes('demo.slow'));

  check('slow operation admitted',(await client.submit('block','fixture','demo.slow',2000)).ok);
  let blocker={};
  for(let i=0;i<100;i++) {
    blocker=await client.result('block');
    if(blocker.state==='running')break;
    await delay(2);
  }
  check('worker occupied',blocker.state==='running');
  check('short-lived queued work admitted',
    (await client.submit('short','fixture','demo.constant',1)).ok);
  await delay(15);
  const short=await client.result('short');
  const currentBlock=await client.result('block');
  check('queued deadline expires while worker remains occupied',
    short.state==='expired' && short.dispatchSequence===0 && currentBlock.state==='running');
  const stats=(await client.snapshot()).stats;
  check('expiration updates pending and metrics before worker releases',
    stats.pending===0 && stats.active===1 && stats.expired>=1);
  check('expired result can be acknowledged immediately',
    (await client.ack('short')).ok && (await client.result('short')).error==='NOT_FOUND');

  for(let i=0;i<100;i++){
    if((await client.result('block')).state==='completed')break;
    await delay(5);
  }
  check('occupied operation completes',(await client.result('block')).state==='completed');
  check('completed operation acknowledged',(await client.ack('block')).ok);

  const exited=new Promise(resolve=>proc.once('close',resolve));
  const shutdownStart=performance.now();
  proc.kill('SIGTERM');
  const exit=await Promise.race([exited,delay(900).then(()=>null)]);
  check('idle daemon exits promptly after SIGTERM',
    exit===0 && performance.now()-shutdownStart<900);
  console.log(JSON.stringify({schema:'tronix.hardening.v1',outcome:'PASS',checks},null,2));
} catch(e) {
  console.error(JSON.stringify({schema:'tronix.hardening.v1',outcome:'FAIL',checks,error:String(e),stderr},null,2));
  process.exitCode=1;
} finally {
  if(proc.exitCode===null)proc.kill('SIGKILL');
  await rm(dir,{recursive:true,force:true});
}
