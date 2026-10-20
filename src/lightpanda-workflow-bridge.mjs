// Bounded adapter connecting native Tronix operations to the existing
// mcc0nnell/lightpanda-agent-bridge's real CDP recorder and replayer.
// Uses one trusted, static, loopback fixture. No caller-provided browser args.
import net from 'node:net';
import {spawn,execFile} from 'node:child_process';
import {createHash} from 'node:crypto';
import {createReadStream} from 'node:fs';
import {lstat,stat,chmod,mkdir,unlink} from 'node:fs/promises';
import {isAbsolute,join,resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {encode,decode} from '@msgpack/msgpack';

const spec=new Map();
for(let i=2;i<process.argv.length;i+=2){
  const k=process.argv[i],v=process.argv[i+1];
  if(!['--root','--binary','--sha256','--python','--agent-bridge','--bridge-commit','--fixture-url'].includes(k)
    ||!v||spec.has(k))throw Error('invalid configuration');
  spec.set(k,v);
}
if(spec.size!==7)throw Error('missing configuration');
const get=k=>spec.get(k);
for(const k of ['--root','--binary','--python','--agent-bridge']){
  if(!isAbsolute(get(k)))throw Error('all paths must be absolute');
}
if(!/^[0-9a-f]{64}$/.test(get('--sha256'))||
   !/^[0-9a-f]{40}$/.test(get('--bridge-commit')))throw Error('pin formats invalid');
const root=resolve(get('--root')),binary=resolve(get('--binary'));
const bridgeDir=resolve(get('--agent-bridge')),python=resolve(get('--python'));
const fixture=new URL(get('--fixture-url'));
if(fixture.protocol!=='http:'||fixture.hostname!=='127.0.0.1'||
   !fixture.port||fixture.pathname!=='/'||fixture.search||fixture.hash||
   fixture.username||fixture.password)throw Error('loopback fixture required');
for(const [path,kind,privateDir] of [
  [root,'directory',true],[bridgeDir,'directory',false],[binary,'file',false]]){
  const st=await lstat(path);
  if(st.isSymbolicLink()||st.uid!==process.getuid()||
     (kind==='directory'?!st.isDirectory():!st.isFile())||
     (privateDir&&(st.mode&0o077)!==0))throw Error('unsafe configured path');
}
// Virtualenv python executables are normally symlinks into root-owned /usr/bin.
const interpreter=await stat(python);
if(!interpreter.isFile()||![process.getuid(),0].includes(interpreter.uid)||
   (interpreter.mode&0o022)!==0)throw Error('unsafe python interpreter');
const sha=createHash('sha256');
for await(const chunk of createReadStream(binary))sha.update(chunk);
if(sha.digest('hex')!==get('--sha256'))throw Error('browser binary SHA-256 mismatch');
const bridgeRevision=await new Promise((yes,no)=>{
  execFile('git',['-C',bridgeDir,'rev-parse','HEAD'],{timeout:3000},(e,out)=>e?no(e):yes(out.trim()));
});
if(bridgeRevision!==get('--bridge-commit'))throw Error('agent bridge commit mismatch');
const dirty=await new Promise((yes,no)=>{
  execFile('git',['-C',bridgeDir,'status','--porcelain','--untracked-files=no'],
    {timeout:3000},(e,out)=>e?no(e):yes(out.trim()));
});
if(dirty)throw Error('agent bridge tracked files modified');
const runs=join(root,'runs');
await mkdir(runs,{mode:0o700});
const runMeta=await lstat(runs);
if(!runMeta.isDirectory()||runMeta.isSymbolicLink()||runMeta.uid!==process.getuid()||
   (runMeta.mode&0o077)!==0)throw Error('unsafe receipt storage');
const socketPath=join(root,'bridge.sock');
if(Buffer.byteLength(socketPath)>100)throw Error('socket path too long');
try{await lstat(socketPath);throw Error('socket already exists');}
catch(e){if(e.code!=='ENOENT')throw e;}
const runner=fileURLToPath(new URL('./lightpanda-workflow-runner.py',import.meta.url));
const frame=message=>{
  const bytes=Buffer.from(encode(message)),prefix=Buffer.alloc(4);
  if(bytes.length>4096)throw Error('oversized response');
  prefix.writeUInt32BE(bytes.length);return Buffer.concat([prefix,bytes]);
};
let running=null,active=false,stopping=false,lastReceipt=null;
function killGroup(child,signal='SIGKILL'){
  if(!child)return;
  try{process.kill(-child.pid,signal);}catch{}
}
async function run(mode){
  if(active)return {ok:false,error:'BUSY'};
  if(mode==='replay'&&!lastReceipt)return {ok:false,error:'NOT_RECORDED'};
  active=true;
  try{
    const stdout=await new Promise((yes,no)=>{
      let data='',done=false;
      const child=spawn(python,[runner,mode,bridgeDir,fixture.href,
        mode==='record'?'-':lastReceipt],{
          cwd:root,detached:true,stdio:['ignore','pipe','pipe'],
          env:{
            PATH:'/usr/bin:/bin',HOME:root,LANG:'C',PYTHONDONTWRITEBYTECODE:'1',
            PYTHONNOUSERSITE:'1',LIGHTPANDA_BINARY:binary,
            LIGHTPANDA_SHA256:get('--sha256'),LIGHTPANDA_RUN_DIR:runs,
            LIGHTPANDA_DISABLE_TELEMETRY:'true',LIGHTPANDA_DISABLE_CORE_DUMP:'1'
          }
        });
      running=child;
      const finish=(err,result)=>{
        if(done)return;
        done=true;clearTimeout(timer);
        if(err)killGroup(child);
        running=null;
        if(err)no(err);else yes(result);
      };
      const timer=setTimeout(()=>finish(Error('workflow timeout')),16000);
      child.once('error',finish);
      child.stdout.on('data',b=>{
        data+=b.toString('utf8');
        if(Buffer.byteLength(data)>8192)finish(Error('oversize workflow output'));
      });
      child.stderr.on('data',()=>{}); // don't expose exception details from pages
      child.once('close',code=>{
        if(code!==0)return finish(Error('workflow execution failed'));
        finish(null,data.trim());
      });
    });
    const result=JSON.parse(stdout);
    if(result.outcome!=='PASS'||!/^[0-9a-f]{32}$/.test(result.receipt_id))
      return {ok:false,error:result.outcome==='FAIL'?'OBSERVATION_MISMATCH':'WORKFLOW_BLOCKED'};
    if(mode==='record'){
      lastReceipt=result.receipt_id;
      console.log('TRONIX_BROWSER_RECEIPT',JSON.stringify({
        kind:'record',receipt_id:result.receipt_id,steps:result.step_count}));
      return {ok:true,value:42};
    }
    if(result.parent_receipt_id!==lastReceipt)return {ok:false,error:'WRONG_PARENT'};
    console.log('TRONIX_BROWSER_RECEIPT',JSON.stringify({
      kind:'replay',receipt_id:result.receipt_id,
      parent_receipt_id:lastReceipt,steps:result.step_count}));
    return {ok:true,value:43};
  }catch{return {ok:false,error:'WORKFLOW_ERROR'};}
  finally{active=false;}
}
const server=net.createServer(socket=>{
  let data=Buffer.alloc(0),called=false;
  socket.setTimeout(18500,()=>socket.destroy());
  socket.on('data',async chunk=>{
    if(called)return;
    data=Buffer.concat([data,chunk]);
    if(data.length>4100){socket.destroy();return;}
    if(data.length<4)return;
    const len=data.readUInt32BE(0);
    if(len===0||len>4096){socket.destroy();return;}
    if(data.length<len+4)return;
    called=true;
    let response={ok:false,error:'DENIED'};
    try{
      if(data.length!==len+4)throw Error('trailing payload');
      const request=decode(data.subarray(4));
      if(request&&Object.getPrototypeOf(request)===Object.prototype&&
         Object.keys(request).sort().join(',')==='op,v'&&request.v===1){
        if(request.op==='browser.record')response=await run('record');
        if(request.op==='browser.replay')response=await run('replay');
      }
    }catch{}
    if(!socket.destroyed)socket.end(frame(response));
  });
});
await new Promise((yes,no)=>{server.once('error',no);server.listen(socketPath,yes);});
await chmod(socketPath,0o600);
console.log('TRONIX_LIGHTPANDA_WORKFLOW_READY',socketPath);
async function stop(){
  if(stopping)return;
  stopping=true;
  killGroup(running);
  server.close();
  try{await unlink(socketPath);}catch(e){if(e.code!=='ENOENT')console.error(e.message);}
  process.exit(0);
}
for(const sig of ['SIGINT','SIGTERM'])process.on(sig,()=>{void stop();});
