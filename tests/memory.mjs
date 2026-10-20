import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {mkdtemp,rm,appendFile,readFile,stat,symlink} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';
const root=resolve(import.meta.dirname,'..');
const dir=await mkdtemp(join(tmpdir(),'tronix-memory-'));
const sock=join(dir,'runtime.sock');
const path=join(dir,'agent.mem');
let proc;
let stderr='';
const client=new TronixClient(sock);
const checks=[];
const check=(name,pass)=>{assert.ok(pass,name);checks.push(name);};
async function start(file=path,extraSocket=sock){
  const p=spawn(join(root,'build/tronixd'),[
    '--socket',extraSocket,'--scheduler',join(root,'build/tronix_scheduler.zip'),
    '--adapter',join(root,'build/tronix_demo.zip'),'--memory',file
  ],{cwd:root,stdio:['ignore','pipe','pipe']});
  let stdout='';stderr='';
  p.stdout.on('data',b=>{stdout+=b.toString();});
  p.stderr.on('data',b=>{stderr+=b.toString();});
  for(let i=0;i<240;i++){
    if(stdout.includes('TRONIX_READY')){proc=p;return;}
    if(p.exitCode!==null)throw Error('daemon startup failed '+stderr);
    await delay(10);
  }
  p.kill('SIGKILL');throw Error('daemon startup timed out '+stderr);
}
async function stop(){
  const p=proc;const closed=new Promise(resolve=>p.once('close',resolve));
  p.kill('SIGTERM');
  if((await Promise.race([closed,delay(1800).then(()=>null)]))===null){
    p.kill('SIGKILL');await closed;
  }
  proc=null;
}
try{
  await start();
  check('owner-private memory file',((await stat(path)).mode&0o077)===0);
  check('missing key returns NOT_FOUND',(await client.memoryGet('agent-a','theme')).error==='NOT_FOUND');
  check('simple memory saved',(await client.memoryPut('agent-a','theme','dark mode')).ok);
  check('stored memory read',(await client.memoryGet('agent-a','theme')).value==='dark mode');
  check('per-agent key separation',(await client.memoryGet('agent-b','theme')).error==='NOT_FOUND');
  check('other agent can store same key',(await client.memoryPut('agent-b','theme','light mode')).ok);
  check('other agent value separate',(await client.memoryGet('agent-b','theme')).value==='light mode');
  check('updating value durable',(await client.memoryPut('agent-a','theme','high contrast')).ok);
  const bad=await client.request('memory_put',{agent:'agent-a',key:'theme',value:'new',command:'bash'});
  check('untrusted parameter rejected',bad.error==='BAD_REQUEST');
  check('memory content with newline rejected',(await client.memoryPut('agent-a','control','hello\nworld')).error==='BAD_REQUEST');
  check('long key rejected',(await client.memoryPut('agent-a','x'.repeat(65),'short')).error==='BAD_REQUEST');
  check('empty value rejected',(await client.memoryPut('agent-a','blank','')).error==='BAD_REQUEST');
  for(let i=0;i<21;i++){
    const x=await client.memoryPut('agent-a','key-'+String(i).padStart(2,'0'),'v'+i);
    assert.equal(x.ok,true,'memory added '+i);
  }
  let first=await client.memoryList('agent-a');
  check('key enumeration bounded to 16',first.keys.length===16&&first.keys[0]==='key-00');
  let next=await client.memoryList('agent-a',first.nextKey);
  check('key enumeration follows cursor',next.keys.includes('theme')&&next.keys[0]==='key-16');
  check('delete known key',(await client.memoryDelete('agent-a','key-20')).ok);
  check('deleted key absent',(await client.memoryGet('agent-a','key-20')).error==='NOT_FOUND');
  check('deleting unknown key fails',(await client.memoryDelete('agent-a','key-20')).ok===false);
  // A simultaneous writer is never permitted to replay a live journal.
  let contender=spawn(join(root,'build/tronixd'),[
    '--socket',join(dir,'other.sock'),'--scheduler',join(root,'build/tronix_scheduler.zip'),
    '--adapter',join(root,'build/tronix_demo.zip'),'--memory',path
  ],{cwd:root,stdio:['ignore','pipe','pipe']});
  let contenderError='';contender.stderr.on('data',b=>contenderError+=b.toString());
  const secondExit=await Promise.race([new Promise(resolve=>contender.once('exit',resolve)),delay(2000).then(()=>null)]);
  if(secondExit===null)contender.kill('SIGKILL');
  check('simultaneous memory writer refused',
        secondExit===2&&contenderError.includes('already held'));
  await stop();
  // Simulate a crash during a trailing record; it must not corrupt committed records.
  await appendFile(path,Buffer.from([0,0,0,42,0x81]));
  await start();
  check('memory replay recovers last committed value',
        (await client.memoryGet('agent-a','theme')).value==='high contrast');
  check('independent agent also survives restart',
        (await client.memoryGet('agent-b','theme')).value==='light mode');
  check('deleted key stays deleted',(await client.memoryGet('agent-a','key-20')).error==='NOT_FOUND');
  check('torn journal tail truncated',(await stat(path)).size===(await readFile(path)).length);
  check('normal scheduler still functions',(await client.ping()).ok);
  console.log(JSON.stringify({schema:'tronix.agent-memory.v1',outcome:'PASS',checks},null,2));
}catch(e){
  console.error(JSON.stringify({schema:'tronix.agent-memory.v1',outcome:'FAIL',checks,error:String(e),stderr},null,2));
  process.exitCode=1;
}finally{
  if(proc)await stop();
  await rm(dir,{recursive:true,force:true});
}
