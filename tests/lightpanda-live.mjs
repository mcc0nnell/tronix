// Live integration of the owned Lightpanda fork with Tronix's native Celix/CDP adapter.
// All browsing is confined to a throwaway HTTP fixture on 127.0.0.1.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createServer} from 'node:http';
import {mkdtemp,rm,lstat} from 'node:fs/promises';
import {join,resolve} from 'node:path';
import {tmpdir} from 'node:os';
import {setTimeout as delay} from 'node:timers/promises';
import {TronixClient} from '../src/client.mjs';

const root=resolve(import.meta.dirname,'..');
const binary=process.env.TRONIX_LIGHTPANDA_BINARY;
const sha=process.env.TRONIX_LIGHTPANDA_SHA256;
if(!binary||!binary.startsWith('/')||!sha||!/^[0-9a-f]{64}$/.test(sha))
  throw Error('Set TRONIX_LIGHTPANDA_BINARY and TRONIX_LIGHTPANDA_SHA256');
const run=await mkdtemp(join(tmpdir(),'tronix-lightpanda-'));
const bridgeSock=join(run,'bridge.sock'),daemonSock=join(run,'runtime.sock');
const server=createServer((req,res)=>{
  if(req.url!=='/'){res.writeHead(404);res.end();return;}
  res.writeHead(200,{'content-type':'text/html; charset=utf-8',
    'cache-control':'no-store'});
  res.end('<!doctype html><html><head><title>Tronix Lightpanda test</title></head><body><p id="tronix-probe">TRONIX_BROWSER_PROBE_OK</p></body></html>');
});
const launched=[];
const evidence={schema:'tronix.lightpanda-live.v1',status:'BLOCKED',
  checks:[],limitations:['Owned local HTTP fixture only',
    'Optional Lightpanda executable pinned to operator-supplied digest',
    'Untrusted website browsing and multi-user isolation not tested',
    'No raw CDP, arbitrary URL or JavaScript operation exposed',
    'Native execution budget remains post-hoc; browser child has a hard timeout']};
function check(name,condition){assert.ok(condition,name);evidence.checks.push(name);}
function launch(command,args,opts={}){
  const proc=spawn(command,args,{cwd:root,stdio:['ignore','pipe','pipe'],...opts});
  const meta={proc,out:'',err:''};
  proc.stdout.on('data',b=>{meta.out+=b.toString();});
  proc.stderr.on('data',b=>{meta.err+=b.toString();});
  launched.push(meta);return meta;
}
async function ready(meta,marker){
  for(let i=0;i<300;i++){
    if(meta.out.includes(marker))return;
    if(meta.proc.exitCode!==null)throw Error(marker+' exited: '+meta.err.slice(-500));
    await delay(20);
  }
  throw Error('not ready: '+marker+' '+meta.err.slice(-500));
}
async function close(meta){
  if(meta.proc.exitCode!==null)return;
  const exited=new Promise(r=>meta.proc.once('exit',r));
  meta.proc.kill('SIGTERM');
  await Promise.race([exited,delay(1500)]);
  if(meta.proc.exitCode===null)meta.proc.kill('SIGKILL');
  await Promise.race([exited,delay(1000)]);
}
async function terminal(client,id){
  for(let i=0;i<300;i++){
    const result=await client.result(id);
    if(['completed','failed','expired','overrun'].includes(result.state))return result;
    await delay(20);
  }
  throw Error('no terminal receipt for '+id);
}
try{
  await new Promise(r=>server.listen(0,'127.0.0.1',r));
  const port=server.address().port;
  const invalidDigest=launch(process.execPath,['src/lightpanda-bridge.mjs','--binary',binary,
    '--sha256','0'.repeat(64),'--fixture-url',`http://127.0.0.1:${port}/`,'--root',run]);
  for(let i=0;i<200&&invalidDigest.proc.exitCode===null;i++)await delay(20);
  check('invalid browser SHA-256 rejected before binding',
    invalidDigest.proc.exitCode!==null&&invalidDigest.proc.exitCode!==0&&
    !invalidDigest.out.includes('TRONIX_LIGHTPANDA_BRIDGE_READY'));
  const remoteURL=launch(process.execPath,['src/lightpanda-bridge.mjs','--binary',binary,
    '--sha256',sha,'--fixture-url','https://example.org/','--root',run]);
  for(let i=0;i<100&&remoteURL.proc.exitCode===null;i++)await delay(20);
  check('non-loopback destination refused',
    remoteURL.proc.exitCode!==null&&remoteURL.proc.exitCode!==0);
  const bridge=launch(process.execPath,['src/lightpanda-bridge.mjs','--binary',binary,
    '--sha256',sha,'--fixture-url',`http://127.0.0.1:${port}/`,'--root',run]);
  await ready(bridge,'TRONIX_LIGHTPANDA_BRIDGE_READY');
  check('Lightpanda backend starts with binary digest validation',true);
  check('bridge socket owner-private',(await lstat(bridgeSock)).mode%512===0o600);
  const runtime=launch(join(root,'build/tronixd'),[
    '--socket',daemonSock,'--scheduler',join(root,'build/tronix_scheduler.zip'),
    '--adapter',join(root,'build/tronix_cdp.zip')
  ],{env:{...process.env,TRONIX_CDP_BRIDGE_SOCKET:bridgeSock}});
  await ready(runtime,'TRONIX_READY');
  const client=new TronixClient(daemonSock);
  const caps=await client.capabilities();
  check('only bounded browser operation exposed',
    caps.ok&&caps.operations.length===1&&caps.operations[0]==='browser.constant');
  for(const op of ['browser.navigate','Runtime.evaluate','browser.click','demo.constant']){
    const denied=await client.submit('reject-'+op,'agent-test',op,1000);
    check('denied '+op,!denied.ok&&denied.error==='DENIED');
  }
  for(let i=0;i<3;i++){
    const id='lightpanda-'+i;
    const queued=await client.submit(id,'agent-'+i,'browser.constant',3000);
    check('admitted fixed local browser probe '+i,queued.ok);
    const result=await terminal(client,id);
    check('executed actual forked Lightpanda '+i,
      result.state==='completed'&&result.value===42&&result.dispatchSequence>0);
    check('acknowledged '+i,(await client.ack(id)).ok);
  }
  const snap=(await client.snapshot()).stats;
  check('native queue drained',snap.pending===0&&snap.active===0&&snap.completed===3&&snap.failed===0);
  evidence.status='PASS';
  evidence.operations=3;
  evidence.browser_sha256=sha;
  console.log(JSON.stringify(evidence,null,2));
}finally{
  for(const proc of launched.reverse())await close(proc);
  await new Promise(r=>server.close(r));
  await rm(run,{recursive:true,force:true});
}
