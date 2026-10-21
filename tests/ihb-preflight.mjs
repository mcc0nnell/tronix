import assert from 'node:assert/strict';
import {createConnection} from 'node:net';
import {spawn} from 'node:child_process';
import {mkdtemp, mkdir, readFile, rm} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {tmpdir} from 'node:os';
import {dirname, join, resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';

const root = resolve(import.meta.dirname, '..');
const ihb = process.env.IHB_NATIVE_ROOT;
if (!ihb) throw Error('Set IHB_NATIVE_ROOT to the approved IHB native_fuzz source tree');
const brokerSocket = (await readFile(join(root, 'build/tronix-ihb-preflight-socket.txt'),'utf8')).trim();
const catalog = join(root,'bridge/catalog.json');
const catalogSHA = createHash('sha256').update(await readFile(catalog)).digest('hex');
const dir=await mkdtemp(join(tmpdir(),'tronix-ihb-preflight-'));
const socket=join(dir,'runtime.sock'),journal=join(dir,'events.msgpack');
await mkdir(dirname(brokerSocket),{recursive:true,mode:0o700});
const broker=spawn('python3',[join(root,'bridge/ihb_preflight.py'),
  '--root',ihb,'--catalog',catalog,'--catalog-sha256',catalogSHA,
  '--socket',brokerSocket,'--allow-uid',String(process.getuid()),'--max-requests','4'],
  {cwd:root,stdio:['ignore','pipe','pipe']});
let brokerOut='',brokerErr='';
broker.stdout.on('data',chunk=>{brokerOut+=chunk.toString()});
broker.stderr.on('data',chunk=>{brokerErr+=chunk.toString()});
const daemon=spawn(join(root,'build/tronixd'),[
  '--socket',socket,'--scheduler',join(root,'build/tronix_scheduler.zip'),
  '--adapter',join(root,'build/tronix_ihb_preflight.zip'),
  '--worker',join(root,'build/tronix_worker'),
  '--journal',journal
],{cwd:root,stdio:['ignore','pipe','pipe']});
let daemonOut='',daemonErr='';
daemon.stdout.on('data',chunk=>{daemonOut+=chunk.toString()});
daemon.stderr.on('data',chunk=>{daemonErr+=chunk.toString()});
const client=new TronixClient(socket);
async function awaitReady(get, proc, label) {
  for(let i=0;i<250;i++) {
    if(get().includes(label))return;
    if(proc.exitCode!==null)throw Error(label+' exited: '+proc.exitCode);
    await delay(20);
  }
  throw Error(label+' not ready: '+get());
}
async function terminal(id) {
  for(let i=0;i<200;i++) {
    const r=await client.result(id);
    if(['completed','failed','overrun','expired'].includes(r.state))return r;
    await delay(20);
  }
  throw Error('missing terminal receipt '+id);
}
function direct(message, split=false) {
  return new Promise((resolveValue,reject)=>{
    const conn=createConnection(brokerSocket); let answer='';
    conn.setTimeout(2500,()=>{conn.destroy();reject(Error('broker timed out'))});
    conn.on('error',reject);
    conn.on('data',chunk=>{answer+=chunk.toString()});
    conn.on('end',()=>resolveValue(answer));
    conn.on('connect',()=>{
      if(split){conn.write(message.slice(0,-1));setTimeout(()=>conn.write('\n'),10);}
      else conn.write(message);
    });
  });
}
const checks=[];
function check(name,ok){assert.ok(ok,name);checks.push(name)}
try {
  await awaitReady(()=>brokerOut,broker,'IHB_PREFLIGHT_READY');
  await awaitReady(()=>daemonOut,daemon,'TRONIX_READY');
  const cap=await client.capabilities();
  check('two named synthetic profiles advertised',
    cap.operations.includes('fuzz.preflight.safe')&&cap.operations.includes('fuzz.preflight.buggy'));
  check('arbitrary commands rejected', (await client.submit('bad','fixture','fuzz.run',3000)).error==='DENIED');
  check('injected command rejected by broker', (await direct('fuzz.preflight.safe;echo hacked\n'))==='ERR\n');
  check('fragmented approved operation accepted', (await direct('fuzz.preflight.safe\n',true))==='OK 1\n');
  for(const [id,op] of [['approved-safe','fuzz.preflight.safe'],['approved-buggy','fuzz.preflight.buggy']]) {
    check(op+' admitted',(await client.submit(id,'fixture',op,3000)).ok);
    const result=await terminal(id);
    check(op+' verified without executing a campaign',result.state==='completed'&&result.value===1);
    check(op+' receipt retained', (await client.receipt(id)).receipt.state==='completed');
  }
  for(let i=0;i<50&&broker.exitCode===null;i++)await delay(20);
  check('bounded broker exits after requests',broker.exitCode===0);
  check('missing broker causes a failed preflight, never a success',
    (await client.submit('offline','fixture','fuzz.preflight.safe',3000)).ok &&
    (await terminal('offline')).state==='failed');
  check('daemon remains healthy',(await client.ping()).ok);
  console.log(JSON.stringify({schema:'tronix.ihb-preflight-acceptance.v0',
    result:'PASS',checks,execution:'read-only preflight; no fuzz campaigns or Docker calls'},null,2));
} catch(err) {
  console.error(JSON.stringify({result:'FAIL',checks,error:String(err),daemonErr,brokerErr},null,2));
  process.exitCode=1;
} finally {
  daemon.kill('SIGTERM'); broker.kill('SIGTERM');
  await Promise.race([delay(2000),
    Promise.all([new Promise(r=>daemon.once('exit',r)),new Promise(r=>broker.once('exit',r))])]);
  if(daemon.exitCode===null)daemon.kill('SIGKILL');
  if(broker.exitCode===null)broker.kill('SIGKILL');
  await rm(dir,{recursive:true,force:true});
}
