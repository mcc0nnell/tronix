// Optional Lightpanda fork backend for Tronix's existing restricted CDP Celix bundle.
// Only the administrator-configured loopback fixture is accessible through this bridge.
// Never accept URLs, scripts, headers or binary paths from a Tronix agent request.
import net from 'node:net';
import {spawn} from 'node:child_process';
import {createReadStream} from 'node:fs';
import {lstat,chmod,unlink} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {resolve,join,isAbsolute} from 'node:path';
import {encode,decode} from '@msgpack/msgpack';

const options={};
for(let i=2;i<process.argv.length;i+=2){
  const flag=process.argv[i],value=process.argv[i+1];
  if(!['--binary','--sha256','--fixture-url','--root'].includes(flag)||
     flag in options||!value)throw Error('expected --binary, --sha256, --fixture-url, --root');
  options[flag]=value;
}
if(Object.keys(options).length!==4)throw Error('missing required bridge option');
const binary=options['--binary'];
const root=options['--root'];
const digest=options['--sha256'];
if(!isAbsolute(binary)||!isAbsolute(root)||!/^[a-f0-9]{64}$/.test(digest))
  throw Error('absolute paths and SHA-256 digest required');
const fixture=new URL(options['--fixture-url']);
if(fixture.protocol!=='http:'||fixture.hostname!=='127.0.0.1'||
   fixture.username||fixture.password||fixture.search||fixture.hash||fixture.pathname!=='/')
  throw Error('fixture must be an explicit local http://127.0.0.1:PORT/ URL');
const [dir,file]=await Promise.all([lstat(root),lstat(binary)]);
if(!dir.isDirectory()||dir.isSymbolicLink()||dir.uid!==process.getuid()||
   (dir.mode&0o077)!==0||!file.isFile()||file.isSymbolicLink())
  throw Error('bridge directory must be private; binary must be a regular file');
const hash=createHash('sha256');
for await(const chunk of createReadStream(binary))hash.update(chunk);
if(hash.digest('hex')!==digest)throw Error('Lightpanda executable digest mismatch');
const socketPath=join(root,'bridge.sock');
if(socketPath.length>100)throw Error('Unix socket path too long');
try{await lstat(socketPath);throw Error('refusing existing bridge socket');}
catch(e){if(e.code!=='ENOENT')throw e;}

function frame(message){
  const data=Buffer.from(encode(message));
  const head=Buffer.alloc(4);head.writeUInt32BE(data.length);
  return Buffer.concat([head,data]);
}
let active=false,stopping=false,current=null;
async function probe(){
  if(active)return {ok:false,error:'BUSY'};
  active=true;
  try{
    // Keep this invocation fixed. The only page is the operator-owned fixture.
    // A hard child-process timeout is independent of Tronix's post-hoc job budget.
    const output=await new Promise((resolvePromise,reject)=>{
      const child=spawn(binary,['fetch','--dump','html','--log-level','error',fixture.href],{
        stdio:['ignore','pipe','pipe'],
        cwd:root,
        // Do not copy tokens, credentials or proxy variables into the browser.
        env:{PATH:process.env.PATH ?? '/usr/bin:/bin',HOME:root,LANG:'C',
          LIGHTPANDA_DISABLE_TELEMETRY:'true',LIGHTPANDA_DISABLE_CORE_DUMP:'1'}
      });
      current=child;
      let stdout='',stderr='',done=false;
      const finish=(err,result)=>{
        if(done)return;done=true;clearTimeout(timer);current=null;
        if(err)reject(err);else resolvePromise(result);
      };
      const timer=setTimeout(()=>{child.kill('SIGKILL');finish(Error('timeout'));},2200);
      child.once('error',e=>finish(e));
      child.stdout.on('data',buf=>{
        stdout+=buf.toString();
        if(Buffer.byteLength(stdout)>65536){child.kill('SIGKILL');finish(Error('oversize'));}
      });
      child.stderr.on('data',buf=>{
        if(stderr.length<4096)stderr+=buf.toString().slice(0,4096-stderr.length);
      });
      child.once('close',code=>{
        if(code!==0)finish(Error('browser exited '+code));
        else finish(null,stdout);
      });
    });
    return /<p id="tronix-probe">TRONIX_BROWSER_PROBE_OK<\/p>/.test(output)
      ? {ok:true,value:42}:{ok:false,error:'PROBE_MISMATCH'};
  }catch{
    return {ok:false,error:'BROWSER_FAILED'};
  }finally{active=false;}
}
const server=net.createServer(socket=>{
  let data=Buffer.alloc(0),handled=false;
  socket.setTimeout(2900,()=>socket.destroy());
  socket.on('data',async chunk=>{
    if(handled)return;
    data=Buffer.concat([data,chunk]);
    if(data.length>4100){socket.destroy();return;}
    if(data.length<4)return;
    const size=data.readUInt32BE(0);
    if(size===0||size>4096){socket.destroy();return;}
    if(data.length<size+4)return;
    handled=true;
    let response={ok:false,error:'DENIED'};
    try{
      if(data.length!==size+4)throw Error('trailing bytes');
      const request=decode(data.subarray(4));
      if(request&&Object.getPrototypeOf(request)===Object.prototype&&
         Object.keys(request).sort().join(',')==='op,v'&&
         request.v===1&&request.op==='browser.constant')response=await probe();
    }catch{}
    if(!socket.destroyed)socket.end(frame(response));
  });
});
await new Promise((yes,no)=>{server.once('error',no);server.listen(socketPath,yes);});
await chmod(socketPath,0o600);
console.log('TRONIX_LIGHTPANDA_BRIDGE_READY',socketPath);
async function stop(){
  if(stopping)return;stopping=true;
  if(current)current.kill('SIGKILL');
  server.close();
  try{await unlink(socketPath);}catch(e){if(e.code!=='ENOENT')console.error(e.message);}
  process.exit(0);
}
for(const signal of ['SIGTERM','SIGINT'])process.on(signal,()=>{void stop();});
