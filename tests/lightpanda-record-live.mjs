// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// End-to-end native Tronix scheduling -> pinned Lightpanda Agent Bridge
// -> live CDP navigation/extraction -> sanitized record/replay receipts.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createServer} from 'node:http';
import {mkdtemp,rm,readdir,readFile,lstat,writeFile,mkdir} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {join,resolve} from 'node:path';
import {tmpdir} from 'node:os';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';

const here=resolve(import.meta.dirname,'..');
const binary=process.env.TRONIX_LIGHTPANDA_BINARY,sha=process.env.TRONIX_LIGHTPANDA_SHA256;
const bridgeDir=process.env.TRONIX_AGENT_BRIDGE_DIR;
const bridgeCommit=process.env.TRONIX_AGENT_BRIDGE_COMMIT;
const python=process.env.TRONIX_AGENT_BRIDGE_PYTHON;
if(!binary?.startsWith('/')||!bridgeDir?.startsWith('/')||!python?.startsWith('/')||
   !/^[0-9a-f]{64}$/.test(sha??'')||!/^[0-9a-f]{40}$/.test(bridgeCommit??''))
  throw Error('Set vetted Lightpanda and Agent Bridge paths, SHA and commit env vars');
const root=await mkdtemp(join(tmpdir(),'tronix-browser-record-'));
const socket=join(root,'runtime.sock'),bridgeSock=join(root,'bridge.sock');
let altered=false;
const server=createServer((req,res)=>{
  const page=req.url==='/'?
    '<h1 id="heading">Tronix Landing</h1>':req.url==='/second'?
    '<h1 id="heading">'+(altered?'CHANGED PAGE':'Tronix Second Page')+'</h1>':null;
  if(!page){res.writeHead(404);res.end();return;}
  res.writeHead(200,{'Content-Type':'text/html','Cache-Control':'no-store'});
  res.end('<!doctype html><html><head><title>Local fixture</title></head><body>'+
    page+'</body></html>');
});
const procs=[];
const report={schema:'tronix.browser-workflow-live.v1',status:'BLOCKED',checks:[],
  limitations:['Loopback fixture only; no arbitrary agent URL/JS or external site',
    'Receipts use unkeyed local SHA-256, not third-party attestation',
    'No multi-tenant sandbox or production credential isolation']};
const check=(name,p)=>{assert.ok(p,name);report.checks.push(name);};
function launch(command,args,opts={}){
  const proc=spawn(command,args,{cwd:here,stdio:['ignore','pipe','pipe'],...opts});
  const meta={proc,out:'',err:''};
  proc.stdout.on('data',b=>{meta.out+=b.toString();});
  proc.stderr.on('data',b=>{meta.err+=b.toString();});
  procs.push(meta);return meta;
}
async function ready(meta,marker){
  for(let i=0;i<350;i++){
    if(meta.out.includes(marker))return;
    if(meta.proc.exitCode!==null)throw Error('startup failed '+meta.err.slice(-600));
    await delay(30);
  }
  throw Error('startup timeout '+marker+' '+meta.err.slice(-500));
}
async function close(meta){
  if(meta.proc.exitCode!==null)return;
  const done=new Promise(r=>meta.proc.once('exit',r));
  meta.proc.kill('SIGTERM');
  await Promise.race([done,delay(1300)]);
  if(meta.proc.exitCode===null)meta.proc.kill('SIGKILL');
  await Promise.race([done,delay(800)]);
}
async function terminal(client,id){
  for(let i=0;i<400;i++){
    const r=await client.result(id);
    if(['completed','failed','expired','overrun'].includes(r.state))return r;
    await delay(40);
  }
  throw Error('not terminal '+id);
}
async function submit(client,id,operation){
  const q=await client.submit(id,'workflow-test',operation,3500);
  check('accepted '+id,q.ok===true);
  return terminal(client,id);
}
async function receipts(){
  const names=(await readdir(join(root,'runs'))).filter(x=>/^[a-f0-9]{32}\.json$/.test(x));
  return Promise.all(names.map(async n=>{
    const txt=await readFile(join(root,'runs',n),'utf8');
    return {name:n,text:txt,data:JSON.parse(txt)};
  }));
}
try{
  await new Promise(r=>server.listen(0,'127.0.0.1',r));
  const url='http://127.0.0.1:'+server.address().port+'/';
  const bridgeArgs=['src/lightpanda-workflow-bridge.mjs','--root',root,'--binary',binary,
    '--sha256',sha,'--python',python,'--agent-bridge',bridgeDir,
    '--bridge-commit',bridgeCommit,'--fixture-url',url];
  async function rejectedConfiguration(args,label){
    const candidate=launch(process.execPath,args);
    for(let i=0;i<180&&candidate.proc.exitCode===null;i++)await delay(25);
    check(label,candidate.proc.exitCode!==null&&candidate.proc.exitCode!==0&&
      !candidate.out.includes('TRONIX_LIGHTPANDA_WORKFLOW_READY'));
  }
  const wrongDigest=[...bridgeArgs];
  wrongDigest[wrongDigest.indexOf('--sha256')+1]='0'.repeat(64);
  await rejectedConfiguration(wrongDigest,
    'incorrect executable digest rejected before socket binding');
  const wrongCommit=[...bridgeArgs];
  wrongCommit[wrongCommit.indexOf('--bridge-commit')+1]='0'.repeat(40);
  await rejectedConfiguration(wrongCommit,
    'incorrect Agent Bridge revision rejected before socket binding');
  const bridge=launch(process.execPath,bridgeArgs);
  await ready(bridge,'TRONIX_LIGHTPANDA_WORKFLOW_READY');
  check('private workflow socket',(await lstat(bridgeSock)).mode%512===0o600);
  const runtime=launch(join(here,'build/tronixd'),[
    '--socket',socket,'--scheduler',join(here,'build/tronix_scheduler.zip'),
    '--adapter',join(here,'build/tronix_lightpanda_record.zip'),
    '--journal',join(root,'events.msgpack')
  ],{env:{...process.env,TRONIX_LIGHTPANDA_WORKFLOW_SOCKET:bridgeSock}});
  await ready(runtime,'TRONIX_READY');
  const client=new TronixClient(socket);
  const caps=await client.capabilities();
  check('only record and replay advertised',
    caps.ok&&caps.operations.join(',')==='browser.record,browser.replay');
  for(const operation of ['browser.navigate','Runtime.evaluate','browser.constant','browser.click']){
    const r=await client.submit('denied-'+operation,'user-agent',operation,3500);
    check('denied '+operation,!r.ok&&r.error==='DENIED');
  }
  const before=await submit(client,'replay-before-record','browser.replay');
  check('replay-before-record fails closed',before.state==='failed');
  const recorded=await submit(client,'record-1','browser.record');
  check('record completed through native Celix',recorded.state==='completed'&&recorded.value===42);
  let files=await receipts();
  check('record stored one sanitized receipt',files.length===1&&files[0].data.kind==='record'&&
    files[0].data.outcome==='PASS'&&files[0].data.steps.length===3);
  check('record excludes raw page text',
    !files[0].text.includes('Tronix Landing')&&!files[0].text.includes('Tronix Second Page'));
  const recordId=files[0].data.receipt_id;
  const reproduced=await submit(client,'replay-1','browser.replay');
  check('native replay succeeded',reproduced.state==='completed'&&reproduced.value===43);
  files=await receipts();
  check('matching replay receipt has parent and steps',
    files.length===2&&files.some(x=>x.data.kind==='replay'&&x.data.outcome==='PASS'&&
    x.data.parent_receipt_id===recordId&&x.data.steps.length===3));
  altered=true;
  const mismatch=await submit(client,'replay-changed','browser.replay');
  check('changed fixture fails hash comparison',mismatch.state==='failed');
  files=await receipts();
  check('mismatch retained a FAIL receipt',
    files.length===3&&files.some(x=>x.data.kind==='replay'&&x.data.outcome==='FAIL'));
  const file=join(root,'runs',recordId+'.json');
  const body=await readFile(file,'utf8');
  await writeFile(file,body.replace('"outcome": "PASS"','"outcome": "FAIL"'),{mode:0o600});
  const tampered=await submit(client,'replay-tampered','browser.replay');
  check('tampered source receipt rejected',tampered.state==='failed');
  const snapshot=(await client.snapshot()).stats;
  check('all jobs accounted and native queue drained',
    snapshot.pending===0&&snapshot.active===0&&snapshot.completed===2&&snapshot.failed===3);
  const history=(await client.events(0,16));
  check('journal enabled for native execution receipts',history.persisted===true);
  await close(runtime);await close(bridge);
  report.status='PASS';
  report.record_id=recordId;
  report.metrics={completed:snapshot.completed,failed:snapshot.failed,
    receipts:files.length,verified_steps:3};
  report.binary_sha256=sha;
  report.agent_bridge_revision=bridgeCommit;
  const rendered=JSON.stringify(report,null,2)+String.fromCharCode(10);
  await mkdir(join(here,'evidence'),{recursive:true});
  await writeFile(join(here,'evidence','lightpanda-record-live.json'),rendered);
  await writeFile(join(here,'evidence','lightpanda-record-live.sha256'),
    createHash('sha256').update(rendered).digest('hex')+'  lightpanda-record-live.json'+String.fromCharCode(10));
  console.log(rendered);
}finally{
  for(const proc of procs.reverse())await close(proc);
  await new Promise(r=>server.close(r));
  await rm(root,{force:true,recursive:true});
}
