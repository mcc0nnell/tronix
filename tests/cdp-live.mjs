// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Live authorized browser fixture: private bridge -> existing gateway -> isolated Chrome target.
// Does not launch or touch a signed-in browser profile.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {setTimeout as delay} from 'node:timers/promises';
import {mkdtemp,rm,writeFile,mkdir} from 'node:fs/promises';
import {join,resolve} from 'node:path';
import {tmpdir} from 'node:os';
import {createHash} from 'node:crypto';
import {TronixClient} from '../src/client.mjs';

const root=resolve(import.meta.dirname,'..');
const modulePath=process.env.TRONIX_GATEWAY_MODULE;
if(!modulePath||!modulePath.startsWith('/'))throw Error('Set TRONIX_GATEWAY_MODULE to an absolute, reviewed gateway.mjs');
const run=await mkdtemp(join(tmpdir(),'tronix-cdp-live-'));
const bridgeSock=join(run,'bridge.sock'),daemonSock=join(run,'runtime.sock');
const paths={scheduler:join(root,'build/tronix_scheduler.zip'),adapter:join(root,'build/tronix_cdp.zip')};
const processes=[];
const ledger={schema:'tronix.cdp-live.v1',checks:[],metrics:{},limitations:[
  'Isolated owned Chrome target on Butcher only',
  'Bridge and Tronix same Unix UID: no multi-tenant isolation',
  'Queue wait deadlines are not execution-time guarantees',
  'No raw CDP access or user-selected operations',
  'Receipts are local and unsigned; not externally immutable'
]};
const check=(name,predicate)=>{assert.ok(predicate,name);ledger.checks.push(name);};
function launch(cmd,args,opts={}) {
  const proc=spawn(cmd,args,{cwd:root,stdio:['ignore','pipe','pipe'],...opts});
  const meta={proc,stdout:'',stderr:''};
  proc.stdout.on('data',b=>{meta.stdout+=b.toString();});
  proc.stderr.on('data',b=>{meta.stderr+=b.toString();});
  processes.push(meta);
  return meta;
}
async function ready(meta,marker) {
  for(let i=0;i<240;i++){
    if(meta.stdout.includes(marker))return;
    if(meta.proc.exitCode!==null)throw Error(marker+' failed exit '+meta.proc.exitCode+' '+meta.stderr.slice(-800));
    await delay(25);
  }
  throw Error(marker+' not ready '+meta.stderr.slice(-800));
}
async function close(meta) {
  if(meta.proc.exitCode!==null)return;
  const settled=new Promise(resolve=>meta.proc.once('exit',resolve));
  meta.proc.kill('SIGTERM');
  await Promise.race([settled,delay(5000)]);
  if(meta.proc.exitCode===null)meta.proc.kill('SIGKILL');
  await Promise.race([settled,delay(1000)]);
}
async function terminal(client,id) {
  for(let i=0;i<350;i++){
    const item=await client.result(id);
    if(['completed','failed','expired'].includes(item.state))return item;
    await delay(8);
  }
  throw Error('No terminal result for '+id);
}
let successCount=0;
try {
  const bridge=launch(process.execPath,['src/cdp-bridge.mjs','--gateway-module',modulePath,'--root',run]);
  await ready(bridge,'TRONIX_CDP_BRIDGE_READY');
  check('reviewed gateway starts owned Chrome target',bridge.stdout.includes('TRONIX_CDP_BRIDGE_READY'));
  const runtime=launch(join(root,'build/tronixd'),[
    '--socket',daemonSock,'--scheduler',paths.scheduler,'--adapter',paths.adapter
  ],{env:{...process.env,TRONIX_CDP_BRIDGE_SOCKET:bridgeSock}});
  await ready(runtime,'TRONIX_READY');
  const client=new TronixClient(daemonSock);
  check('native Celix CDP bundle starts', (await client.ping()).runtime==='tronix/0.2');
  const caps=await client.capabilities();
  check('only browser observation capability advertised',
    caps.ok&&caps.operations.length===1&&caps.operations[0]==='browser.constant');
  for(const operation of ['Runtime.evaluate','control.navigateLab','demo.constant','browser.navigate']){
    const denied=await client.submit('denied-'+operation,'test',operation,1500);
    check('denied '+operation,denied.ok===false && denied.error==='DENIED');
  }
  const sample=await client.submit('browser-0','agent-a','browser.constant',2500);
  check('authorized browser request admitted',sample.ok);
  const initial=await terminal(client,'browser-0');
  check('authorized Celix operation reached real CDP and returned 42',
    initial.state==='completed'&&initial.value===42&&initial.dispatchSequence>0);
  successCount++;
  check('successful result acknowledged',(await client.ack('browser-0')).ok);
  for(let i=0;i<20;i++){
    const id='browser-'+(i+1);
    const out=await client.submit(id,'agent-'+(i%4),'browser.constant',2500);
    assert.equal(out.ok,true);
    const item=await terminal(client,id);
    assert.equal(item.state,'completed');
    assert.equal(item.value,42);
    successCount++;
    assert.equal((await client.ack(id)).ok,true);
  }
  check('21 live read-only CDP observations completed',successCount===21);
  const snap=(await client.snapshot()).stats;
  ledger.metrics={observations:successCount,native:snap,chrome:'dedicated loopback Chrome 152 fixture'};
  check('single native command in flight and queue drained',snap.active===0&&snap.pending===0&&snap.completed===21&&snap.failed===0);
  await close(runtime);
  await close(bridge);
  const marker=bridge.stdout.match(/TRONIX_CDP_BRIDGE_END (\{[^\n]+\})/);
  check('gateway emits completed execution accounting',!!marker);
  const account=JSON.parse(marker[1]);
  ledger.metrics.gateway={forwarded:account.forwarded,denied:account.denied,
    outcomes:account.outcomes};
  check('exact authorized CDP forwarding; denies did not reach Chrome',
    account.forwarded===successCount && account.denied===0 &&
    account.outcomes.length===successCount &&
    account.outcomes.every(x=>x==='EXECUTED'));
  ledger.outcome='PASS';
  await mkdir(join(root,'evidence'),{recursive:true});
  const receipt=JSON.stringify(ledger,null,2)+'\n';
  await writeFile(join(root,'evidence','cdp-live.json'),receipt);
  await writeFile(join(root,'evidence','cdp-live.sha256'),
    createHash('sha256').update(receipt).digest('hex')+'  cdp-live.json\n');
  console.log(JSON.stringify(ledger,null,2));
} finally {
  for(const meta of processes.reverse()) await close(meta);
  await rm(run,{recursive:true,force:true});
}
