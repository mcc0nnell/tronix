// SPDX-License-Identifier: Apache-2.0
// Exact typed MessagePack v2 client for the standalone opt-in verifier.
import net from 'node:net';
import {encode,decode} from '@msgpack/msgpack';
const MAX_REQUEST=64*1024+1024;
const MAX_REPLY=2048;
export function verifyRequest(socketPath, taskId, bytes) {
  if (!(bytes instanceof Uint8Array)) throw Error('binary answer required');
  return sendVerification(socketPath,{v:2,action:'verify',taskId,answer:bytes});
}
export function verifyStats(socketPath) {
  return sendVerification(socketPath,{v:2,action:'stats'});
}
export function sendVerification(socketPath,request,timeoutMs=2500) {
  const payload=Buffer.from(request instanceof Uint8Array?request:encode(request));
  if(!socketPath.startsWith('/')||!payload.length||payload.length>MAX_REQUEST)
    return Promise.reject(Error('unsafe socket or request size'));
  return new Promise((resolve,reject)=>{
    const sock=net.createConnection(socketPath);
    let done=false,buffer=Buffer.alloc(0),size=-1;
    const finish=(error,value)=>{
      if(done)return;done=true;sock.destroy();
      if(error)reject(error);else resolve(value);
    };
    sock.setTimeout(timeoutMs,()=>finish(Error('verifier timeout')));
    sock.on('error',e=>finish(e));
    sock.on('close',()=>finish(Error('verifier did not return a complete reply')));
    sock.on('connect',()=>{
      const prefix=Buffer.alloc(4);prefix.writeUInt32BE(payload.length);
      sock.write(Buffer.concat([prefix,payload]));
    });
    sock.on('data',chunk=>{
      buffer=Buffer.concat([buffer,chunk]);
      if(buffer.length>MAX_REPLY+4)return finish(Error('overlarge reply'));
      if(size===-1&&buffer.length>=4){
        size=buffer.readUInt32BE(0);
        if(size<1||size>MAX_REPLY)return finish(Error('invalid reply length'));
      }
      if(size!==-1&&buffer.length>=size+4){
        if(buffer.length!==size+4)return finish(Error('trailing reply bytes'));
        let parsed;try{parsed=decode(buffer.subarray(4));}catch(e){return finish(e);}
        if(!parsed||parsed.v!==2||typeof parsed.ok!=='boolean')
          return finish(Error('invalid typed reply'));
        finish(null,parsed);
      }
    });
  });
}
