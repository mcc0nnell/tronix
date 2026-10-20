// Optional Node client for the native Tronix MessagePack v1 wire protocol.
import net from 'node:net';
import {encode,decode} from '@msgpack/msgpack';
const LIMIT=4096;
export class TronixClient {
  constructor(socketPath){
    if(typeof socketPath!=='string'||!socketPath.startsWith('/'))throw Error('Absolute Unix socket path required');
    this.socketPath=socketPath;
  }
  request(action,fields={}){
    const body=Buffer.from(encode({v:1,action,...fields}));
    if(body.length===0||body.length>LIMIT)throw Error('FRAME_TOO_LARGE');
    return new Promise((resolve,reject)=>{
      let done=false,buffer=Buffer.alloc(0);
      const socket=net.createConnection(this.socketPath);
      const finish=(err,value)=>{
        if(done)return;done=true;socket.destroy();if(err)reject(err);else resolve(value);
      };
      socket.setTimeout(3000,()=>finish(Error('Tronix IPC timeout')));
      socket.on('error',e=>finish(e));
      socket.on('close',()=>finish(Error('Tronix socket closed before response')));
      socket.on('connect',()=>{
        const header=Buffer.alloc(4);header.writeUInt32BE(body.length);
        socket.write(Buffer.concat([header,body]));
      });
      socket.on('data',chunk=>{
        buffer=Buffer.concat([buffer,chunk]);
        if(buffer.length>LIMIT+4)return finish(Error('FRAME_TOO_LARGE'));
        if(buffer.length<4)return;
        const size=buffer.readUInt32BE(0);
        if(size===0||size>LIMIT)return finish(Error('BAD_FRAME'));
        if(buffer.length>=4+size){
          try{finish(null,decode(buffer.subarray(4,4+size)));}
          catch(e){finish(e);}
        }
      });
    });
  }
  ping(){return this.request('ping');}
  capabilities(){return this.request('capabilities');}
  submit(id,agent,operation,deadlineMs=2000){
    return this.request('submit',{id,agent,operation,deadlineMs});
  }
  result(id){return this.request('result',{id});}
  ack(id){return this.request('ack',{id});}
  snapshot(){return this.request('snapshot');}
  events(after=0,limit=16){return this.request('events',{after,limit});}
  receipt(id){return this.request('receipt',{id});}
  memoryPut(agent,key,value){return this.request('memory_put',{agent,key,value});}
  memoryGet(agent,key){return this.request('memory_get',{agent,key});}
  memoryDelete(agent,key){return this.request('memory_delete',{agent,key});}
  memoryList(agent,afterKey=''){return this.request('memory_list',{agent,afterKey});}
}
