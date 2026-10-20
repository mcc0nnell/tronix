// Linux Landlock v1: allowed Celix execution, denied host-file reads/writes
// and denied TCP connect. This test intentionally uses a real listener.
import assert from 'node:assert/strict';
import net from 'node:net';
import {spawn} from 'node:child_process';
import {mkdtemp,writeFile,readFile,access,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';

const root=resolve(import.meta.dirname,'..');
const dir=await mkdtemp(join(tmpdir(),'tronix-landlock-'));
const sock=join(dir,'daemon.sock');
const secret=join(dir,'host-secret.txt'),forbidden=join(dir,'forbidden.txt');
await writeFile(secret,'HOST PRIVATE CONTENT',{mode:0o600});
const listener=net.createServer(socket=>socket.destroy());
await new Promise((resolve,reject)=>{
  listener.once('error',reject);
  listener.listen(0,'127.0.0.1',resolve);
});
const port=listener.address().port;
const checks=[];
const check=(label,result)=>{assert.ok(result,label);checks.push(label);};
const client=new TronixClient(sock);
const p=spawn(join(root,'build/tronixd'),[
  '--socket',sock,'--scheduler',join(root,'build/tronix_scheduler.zip'),
  '--adapter',join(root,'build/tronix_demo.zip'),
  '--worker',join(root,'build/tronix_worker'),
  '--sandbox','landlock',
  '--journal',join(dir,'events.mp'),
  '--memory',join(dir,'memory.mp')
],{cwd:root,env:{
  ...process.env,TRONIX_ISOLATION_FIXTURES:'1',
  TRONIX_READ_PROBE:secret,TRONIX_WRITE_PROBE:forbidden,
  TRONIX_CONNECT_PORT:String(port)
},stdio:['ignore','pipe','pipe']});
let output='',stderr='';
p.stdout.on('data',b=>output+=b.toString());
p.stderr.on('data',b=>stderr+=b.toString());
async function until(id){
  for(let i=0;i<200;i++){
    const r=await client.result(id);
    if(['completed','failed','expired','overrun'].includes(r.state))return r;
    await delay(10);
  }
  throw Error('operation did not finish '+id);
}
async function execute(id,op){
  const admitted=await client.submit(id,'sandbox-agent',op,2000);
  check(op+' admitted',admitted.ok);
  const result=await until(id);
  await client.ack(id);
  return result;
}
try{
  for(let i=0;i<250&&!output.includes('TRONIX_READY');i++){
    if(p.exitCode!==null)throw Error('sandbox startup failed '+p.exitCode+' '+stderr);
    await delay(10);
  }
  check('Landlock ABI 4 admission inspection started',output.includes('TRONIX_READY'));
  check('normal Celix work survives filesystem restriction',
        (await execute('allowed','demo.constant')).value===42);
  const read=await execute('read-denied','demo.read_probe');
  check('private host file read returns EACCES',
        read.state==='completed'&&read.value===13);
  check('secret unchanged',(await readFile(secret,'utf8'))==='HOST PRIVATE CONTENT');
  const write=await execute('write-denied','demo.write_probe');
  check('host file creation outside cache returns EACCES',
        write.state==='completed'&&write.value===13);
  let exists=true;
  try{await access(forbidden);}catch{exists=false;}
  check('unauthorized file absent',!exists);
  const network=await execute('net-denied','demo.net_probe');
  check('TCP connect to real loopback listener returns EACCES',
        network.state==='completed'&&network.value===13);
  check('unapproved commands remain denied',
        (await client.submit('shell','sandbox-agent','bash')).error==='DENIED');
  // Unsupported sandbox backends/unsupervised mode must never downgrade
  // to the unrestricted execution path.
  async function refused(extra) {
    const child=spawn(join(root,'build/tronixd'),[
      '--socket',join(dir,'rejected-'+Math.random().toString(36).slice(2)+'.sock'),
      '--scheduler',join(root,'build/tronix_scheduler.zip'),
      '--adapter',join(root,'build/tronix_demo.zip'),...extra
    ],{cwd:root,stdio:['ignore','pipe','pipe']});
    let out='';
    child.stdout.on('data',d=>out+=d.toString());
    const code=await Promise.race([
      new Promise(resolve=>child.once('close',resolve)),
      delay(2500).then(()=>null)
    ]);
    if(code===null)child.kill('SIGKILL');
    return code===2 && !out.includes('TRONIX_READY');
  }
  check('Landlock requires separate supervised worker',
        await refused(['--sandbox','landlock']));
  check('unsupported Bubblewrap backend fails closed',
        await refused(['--worker',join(root,'build/tronix_worker'),
                       '--sandbox','bubblewrap']));

  check('memory works independently of child Landlock',
        (await client.memoryPut('sandbox-agent','note','persist')).ok &&
        (await client.memoryGet('sandbox-agent','note')).value==='persist');
  const receipt=await client.receipt('read-denied');
  check('receipt retains measured denied access',receipt.ok&&receipt.receipt.value===13);
  console.log(JSON.stringify({schema:'tronix.landlock.v1',outcome:'PASS',checks,
    observedReadErrno:read.value,observedWriteErrno:write.value,
    observedConnectErrno:network.value},null,2));
}catch(e){
  console.error(JSON.stringify({schema:'tronix.landlock.v1',outcome:'FAIL',
    checks,error:String(e),stderr:stderr.slice(-4000)},null,2));
  process.exitCode=1;
}finally{
  const closed=new Promise(resolve=>p.once('close',resolve));
  p.kill('SIGTERM');
  if((await Promise.race([closed,delay(2500).then(()=>null)]))===null){
    p.kill('SIGKILL');await closed;
  }
  await new Promise(resolve=>listener.close(resolve));
  await rm(dir,{recursive:true,force:true});
}
