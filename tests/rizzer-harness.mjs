// SPDX-License-Identifier: Apache-2.0
// Bounded end-to-end TRONIX scheduler/grants -> Celix adapter ->
// owner-private bridge -> reviewed Rizzer native harness -> signed grant evidence.
// Does not contact live targets, APIs, browsers or network services.
import assert from 'node:assert/strict';
import {createHash,randomBytes} from 'node:crypto';
import {spawn,spawnSync} from 'node:child_process';
import {mkdtemp,readFile,writeFile,mkdir,chmod,stat,access,rm} from 'node:fs/promises';
import {join,resolve,dirname} from 'node:path';
import {tmpdir} from 'node:os';
import net from 'node:net';
import {encode,decode} from '@msgpack/msgpack';
import {setTimeout as sleep} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';

const REPO=resolve(import.meta.dirname,'..');
const SHA=bytes=>createHash('sha256').update(bytes).digest('hex');
const paths=[
  'RIZZER_HARNESS_TEST_CLI','RIZZER_HARNESS_TEST_CASEBOOK',
  'RIZZER_HARNESS_TEST_HOST','RIZZER_HARNESS_TEST_BUNDLE'
];
const configured=paths.every(x=>process.env[x]);
if(!configured) {
  const check=spawnSync(process.execPath,['--check',join(REPO,'scripts/rizzer_harness_sidecar.mjs')],
                         {encoding:'utf8'});
  assert.equal(check.status,0,'sidecar JavaScript syntax must validate');
  console.log('TRONIX_RIZZER_HARNESS_TEST: bridge syntax PASS; native integration SKIPPED (explicit pinned Rizzer artifacts not configured)');
  process.exit(0);
}
const root=process.env.RIZZER_HARNESS_TEST_ROOT ?? await mkdtemp(join(tmpdir(),'tronix-rizzer-harness-'));
if(process.env.RIZZER_HARNESS_TEST_ROOT){
  await mkdir(root,{mode:0o700});
  await chmod(root,0o700);
}
const evidenceDir=join(root,'sidecar-evidence');
await mkdir(evidenceDir,{mode:0o700});
const bridgeRoot='/tmp/tronix-rizzer-harness-'+process.getuid();
// Refuse to disturb an existing owner-operated bridge. The test owns this
// directory only after an exclusive mkdir and removes only what it created.
await mkdir(bridgeRoot,{mode:0o700});
const tokenFile=join(bridgeRoot,'bridge.token');
await writeFile(tokenFile,randomBytes(32).toString('hex'),{mode:0o600});
const socket=join(bridgeRoot,'harness.sock');
const tronixSocket=join(root,'tronix.sock');
const grants=join(root,'grants.conf');
const evidence=join(root,'grant-evidence.txt');
const evidenceKey=join(root,'hmac.key');
await writeFile(evidenceKey,randomBytes(32),{mode:0o600});
const adapter=join(REPO,'build/tronix_rizzer_harness.zip');
const scheduler=join(REPO,'build/tronix_scheduler.zip');
const worker=join(REPO,'build/tronix_worker');
const daemon=join(REPO,'build/tronixd');
const adapterSha=SHA(await readFile(adapter));
const cli=process.env.RIZZER_HARNESS_TEST_CLI;
const casebook=process.env.RIZZER_HARNESS_TEST_CASEBOOK;
const expectedCount=JSON.parse(await readFile(join(casebook,'ir.json'),'utf8')).cases.length;
assert.ok(expectedCount>=3&&expectedCount<=32,'bounded reference casebook');
const host=process.env.RIZZER_HARNESS_TEST_HOST;
const bundle=process.env.RIZZER_HARNESS_TEST_BUNDLE;
const sideArgs=['scripts/rizzer_harness_sidecar.mjs',
  '--socket',socket,'--token-file',tokenFile,
  '--rizzer-cli',cli,'--rizzer-cli-sha256',SHA(await readFile(cli)),
  '--casebook',casebook,
  '--host',host,'--host-sha256',SHA(await readFile(host)),
  '--bundle',bundle,'--bundle-sha256',SHA(await readFile(bundle)),
  '--evidence-dir',evidenceDir];
let side=null,live=null;
let sideOut='',sideErr='',daemonOut='',daemonErr='';
let cleanupTasks=[];
const checks=[];
const pass=(msg,predicate)=>{assert.ok(predicate,msg);checks.push(msg)};
async function awaitReady(proc,ready,output,{loops=180}={}) {
  for(let i=0;i<loops;i++){
    if(output().includes(ready))return;
    if(proc.exitCode!==null||proc.signalCode!==null)break;
    await sleep(30);
  }
  throw Error('server did not become ready: '+ready+' output '+output().slice(-1100));
}
async function stop(proc) {
  if(!proc)return;
  if(proc.exitCode===null&&proc.signalCode===null)proc.kill('SIGTERM');
  for(let i=0;i<70;i++){
    if(proc.exitCode!==null||proc.signalCode!==null)return;
    await sleep(30);
  }
  if(proc.exitCode===null&&proc.signalCode===null)proc.kill('SIGKILL');
}
async function sendRaw(body) {
  return new Promise((done,reject)=>{
    const c=net.createConnection(socket);
    c.setTimeout(2000,()=>{c.destroy();reject(Error('socket timeout'))});
    let buffer=Buffer.alloc(0);
    c.on('error',reject);
    c.on('connect',()=>{
      const encoded=Buffer.from(encode(body));
      const size=Buffer.alloc(4);
      size.writeUInt32BE(encoded.length,0);
      c.write(Buffer.concat([size,encoded]));
    });
    c.on('data',b=>{
      buffer=Buffer.concat([buffer,b]);
      if(buffer.length>=4){
        const n=buffer.readUInt32BE(0);
        if(n>4096)return reject(Error('oversized bridge reply'));
        if(buffer.length>=4+n){done(decode(buffer.subarray(4,4+n)));c.destroy()}
      }
    });
  });
}
async function resultEventually(client,id) {
  for(let i=0;i<700;i++){
    const response=await client.result(id);
    if(['completed','failed','overrun','expired'].includes(response.state))return response;
    await sleep(15);
  }
  throw Error('TRONIX job never reached terminal state');
}
try {
  side=spawn(process.execPath,sideArgs,{cwd:REPO,env:{...process.env},
                                       stdio:['ignore','pipe','pipe']});
  side.stdout.on('data',b=>sideOut+=b.toString());
  side.stderr.on('data',b=>sideErr+=b.toString());
  await awaitReady(side,'RIZZER_HARNESS_BRIDGE_READY',()=>sideOut+'\n'+sideErr);
  pass('fixed-scope sidecar binds owner-private socket',(await stat(socket)).mode&0o077 ? false:true);
  const denied=await sendRaw({v:1,op:'rizzer.harness.reference',token:'0'.repeat(64)});
  pass('untrusted client with incorrect token cannot execute',denied.ok===false&&denied.value===0);
  const wrongOp=await sendRaw({v:1,op:'shell.command',token:(await readFile(tokenFile)).toString()});
  pass('even authenticated socket client cannot choose arbitrary operation',wrongOp.ok===false);
  pass('denied direct requests did not create Rizzer evidence',(await (await import('node:fs/promises')).readdir(evidenceDir)).length===0);
  const grant=['tronix.grants.v1','bundle org.tronix.adapter.rizzer_harness',
               'version 0.1.0','artifact '+adapterSha,'allow rizzer.harness.reference',''].join('\n');
  await writeFile(grants,grant,{mode:0o600});
  live=spawn(daemon,['--socket',tronixSocket,'--scheduler',scheduler,'--adapter',adapter,
    '--worker',worker,'--adapter-sha256',adapterSha,'--grants',grants,
    '--evidence',evidence,'--evidence-key',evidenceKey],
    {cwd:REPO,env:{...process.env},
     stdio:['ignore','pipe','pipe']});
  live.stdout.on('data',b=>daemonOut+=b.toString());
  live.stderr.on('data',b=>daemonErr+=b.toString());
  await awaitReady(live,'TRONIX_READY',()=>daemonOut+'\n'+daemonErr);
  const client=new TronixClient(tronixSocket);
  const caps=await client.capabilities();
  pass('TRONIX advertises exactly operator-granted harness capability',
       caps.ok&&caps.operations.length===1&&caps.operations[0]==='rizzer.harness.reference');
  const prohibited=await client.submit('reject-target','fixture','thread.nodes_up',2000);
  pass('TRONIX denies operation not in grant',prohibited.error==='DENIED');
  const submitted=await client.submit('harness-first','fixture','rizzer.harness.reference',11000);
  pass('TRONIX scheduler admits permitted harness operation',submitted.ok===true);
  const result=await resultEventually(client,'harness-first');
  pass('TRONIX supervised worker completed full pinned native harness',
       result.state==='completed'&&result.value===expectedCount);
  const dispatch=JSON.parse(await readFile(join(evidenceDir,'run-0001.dispatch.json'),'utf8'));
  pass('bridge captured reviewed native/Celix parity and no target execution',
       dispatch.schema==='tronix.rizzer.sidecar.v1'&&dispatch.offline_only===true&&
       dispatch.target_executed===false&&dispatch.receipt.count===expectedCount);
  const receipt=JSON.parse(await readFile(join(evidenceDir,'run-0001/receipt.json'),'utf8'));
  pass('underlying Rizzer native bridge generated source-bound evidence',
       receipt.status==='done'&&receipt.outputs[0].reference_parity===true&&
       receipt.outputs[0].target_executed===false);
  const verified=spawnSync('python3',['scripts/verify_grant_evidence.py',
    '--key',evidenceKey,'--grants',grants,'--adapter',adapter,
    '--evidence',evidence],{cwd:REPO,encoding:'utf8',timeout:10000});
  pass('TRONIX independent checker verifies HMAC-linked grant evidence',
       verified.status===0&&JSON.parse(verified.stdout).verified===1);
  console.log('TRONIX_RIZZER_HARNESS_TEST_PASS',JSON.stringify({
    tests:checks.length,run:'harness-first',state:result.state,value:result.value,
    native_cases:dispatch.receipt.count,offline:true,production_target_executed:false,
    grant_evidence_verified:1,
    evidence_root:root
  }));
} catch(err) {
  console.error('TRONIX_RIZZER_HARNESS_TEST_FAIL',err.stack??err.message);
  console.error('DAEMON_STDERR_LAST',daemonErr.slice(-1400));
  console.error('SIDECAR_STDERR_LAST',sideErr.slice(-1400));
  process.exitCode=1;
} finally {
  await stop(live);
  await stop(side);
  await rm(bridgeRoot,{force:true,recursive:true});
  if(!process.env.RIZZER_HARNESS_TEST_ROOT && process.exitCode!==1){
    await rm(root,{force:true,recursive:true});
  }
}
