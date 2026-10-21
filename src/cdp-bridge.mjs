// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Local, deliberately narrow bridge between Tronix's Celix bundle and the
// existing WindAnvil CDP gateway. Owns one disposable Chrome target.
import net from 'node:net';
import {pathToFileURL} from 'node:url';
import {join,resolve} from 'node:path';
import {lstat,chmod} from 'node:fs/promises';
import {encode,decode} from '@msgpack/msgpack';

const args=process.argv.slice(2);
if(args.length!==4||args[0]!=='--gateway-module'||args[2]!=='--root'){
  console.error('usage: cdp-bridge.mjs --gateway-module /absolute/gateway.mjs --root /private/dir');
  process.exit(2);
}
const gatewayModule=resolve(args[1]);
const root=resolve(args[3]);
const meta=await lstat(root);
if(!meta.isDirectory()||meta.uid!==process.getuid()||(meta.mode&0o077)!==0)
  throw Error('Bridge root must be owned and mode 0700');
const {Gateway}=await import(pathToFileURL(gatewayModule).href);
const gateway=new Gateway(join(root,'gateway'));
await gateway.start();
const socketPath=join(root,'bridge.sock');
const frame=x=>{
  const b=Buffer.from(encode(x)),h=Buffer.alloc(4);
  if(b.length===0||b.length>4096)throw Error('bad bridge reply');
  h.writeUInt32BE(b.length);return Buffer.concat([h,b]);
};
let server;
let stopped=false;
async function shutdown(){
  if(stopped)return;
  stopped=true;
  if(server)await new Promise(r=>server.close(r));
  await gateway.stop();
  console.log('TRONIX_CDP_BRIDGE_END',JSON.stringify({forwarded:gateway.forwarded,denied:gateway.denied,
    outcomes:gateway.audit.map(x=>x.outcome)}));
}
server=net.createServer(socket=>{
  let data=Buffer.alloc(0);let called=false;
  socket.setTimeout(3000,()=>socket.destroy());
  socket.on('data',async chunk=>{
    if(called)return;
    data=Buffer.concat([data,chunk]);
    if(data.length>4100){socket.destroy();return;}
    if(data.length<4)return;
    const size=data.readUInt32BE(0);
    if(size===0||size>4096){socket.destroy();return;}
    if(data.length<size+4)return;
    called=true;
    let response={ok:false,error:'DENIED'};
    try{
      const request=decode(data.subarray(4,4+size));
      if(request && Object.getPrototypeOf(request)===Object.prototype &&
        Object.keys(request).sort().join(',')==='op,v' &&
        request.v===1&&request.op==='browser.constant'){
        // Observer token never leaves this process, and operation arguments
        // are fixed by WindAnvil's own admission policy.
        const result=await gateway.handle({op:'observe.constant',token:gateway.observer});
        if(result.ok===true && result.value===42)response={ok:true,value:42};
        else response={ok:false,error:'CDP_FAILED'};
      }
    }catch{}
    if(!socket.destroyed)socket.end(frame(response));
  });
});
server.on('error',e=>{console.error('TRONIX_CDP_BRIDGE_ERROR',e.message);process.exitCode=1;});
await new Promise((res,rej)=>{server.once('error',rej);server.listen(socketPath,res);});
await chmod(socketPath,0o600);
console.log('TRONIX_CDP_BRIDGE_READY',socketPath);
for(const sig of ['SIGTERM','SIGINT']){
  process.on(sig,()=>{void shutdown().then(()=>process.exit(0),e=>{
    console.error('shutdown failed',e.message);process.exit(1);});});
}
