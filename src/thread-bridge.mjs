// Local-only, read-only telemetry from two real OpenThread POSIX simulated nodes.
// No arbitrary commands, addresses, network keys, or user-supplied scripts accepted.
import net from 'node:net';
import {spawn} from 'node:child_process';
import {existsSync,statSync,chmodSync,unlinkSync} from 'node:fs';
import {dirname} from 'node:path';
import {encode,decode} from '@msgpack/msgpack';

const socketPath=process.env.TRONIX_THREAD_BRIDGE_SOCKET;
const cliPath=process.env.OT_CLI_PATH;
const base=Number(process.env.THREAD_SIM_BASE_ID ?? 21);
if(!socketPath?.startsWith('/') || !cliPath?.startsWith('/') ||
   !Number.isInteger(base)||base<1||base>31 ||
   !existsSync(cliPath) || existsSync(socketPath)) {
  console.error('set private absolute TRONIX_THREAD_BRIDGE_SOCKET, OT_CLI_PATH; socket must not exist');
  process.exit(2);
}
const dir=statSync(dirname(socketPath));
if(!dir.isDirectory() || dir.uid!==process.getuid() || (dir.mode&0o077)!==0){
  console.error('socket directory must be private (0700) and owned by process UID');
  process.exit(2);
}
class Node {
  constructor(id) {
    this.id=id;this.pending=null;this.buffer='';this.closed=false;
    this.queue=Promise.resolve();
    this.child=spawn(cliPath,[String(id)],{stdio:['pipe','pipe','pipe']});
    this.child.stdout.on('data',b=>this.onData(b.toString()));
    this.child.stdin.on('error',()=>{ if(this.pending)this.pending.reject(new Error('CLI stdin closed')); });
    this.child.stderr.on('data',b=>process.stderr.write('ot-'+id+': '+b.toString().slice(0,1024)));
    this.child.on('error',err=>{this.closed=true;if(this.pending)this.pending.reject(err);});
    this.child.on('exit',()=>{this.closed=true;
      if(this.pending)this.pending.reject(new Error('node exited'));
    });
  }
  onData(chunk) {
    this.buffer+=chunk;
    if(this.buffer.length>16384)this.buffer=this.buffer.slice(-4096);
    let pos;
    while((pos=this.buffer.indexOf('\n'))!==-1){
      const line=this.buffer.slice(0,pos).replace(/\r/g,'').trim().replace(/^>\s*/,'');
      this.buffer=this.buffer.slice(pos+1);
      if(!this.pending)continue;
      if(line==='Done'){
        const p=this.pending;this.pending=null;clearTimeout(p.timer);p.resolve(p.lines);
      }else if(/^Error \d+:/.test(line)){
        const p=this.pending;this.pending=null;clearTimeout(p.timer);p.reject(new Error(line));
      }else if(line && line!==this.pending.command)this.pending.lines.push(line);
    }
  }
  command(command,timeout=5000){
    const run=()=>new Promise((resolve,reject)=>{
      if(this.closed)return reject(new Error('node closed'));
      const timer=setTimeout(()=>{this.pending=null;this.closed=true;this.child.kill('SIGTERM');reject(new Error('CLI timed out: '+command));},timeout);
      this.pending={resolve,reject,lines:[],timer,command};
      this.child.stdin.write(command+'\n');
    });
    const out=this.queue.then(run);
    this.queue=out.catch(()=>{});
    return out;
  }
  shutdown(){this.child.kill('SIGTERM');}
}
const delay=ms=>new Promise(r=>setTimeout(r,ms));
const nodes=[new Node(base),new Node(base+1)];
let server=null;
async function role(node){return (await node.command('state'))[0]?.trim();}
async function waitFor(predicate,timeout=30000){
  const until=Date.now()+timeout;
  while(Date.now()<until){
    try {if(await predicate())return;} catch {}
    await delay(500);
  }
  throw new Error('Thread mesh did not converge within '+timeout+'ms');
}
async function init() {
  await nodes[0].command('dataset init new');
  await nodes[0].command('dataset commit active');
  await nodes[0].command('ifconfig up');
  await nodes[0].command('thread start');
  await waitFor(async()=>await role(nodes[0])==='leader');
  const hex=(await nodes[0].command('dataset active -x'))[0];
  if(!/^[0-9a-f]{40,512}$/i.test(hex))throw new Error('invalid operational dataset');
  await nodes[1].command('dataset set active '+hex);
  await nodes[1].command('ifconfig up');
  await nodes[1].command('thread start');
  await waitFor(async()=>{
    const r=await role(nodes[1]);return r==='child'||r==='router';
  });
}
function reply(socket,obj){
  if(socket.destroyed)return;
  const b=Buffer.from(encode(obj));
  const n=Buffer.alloc(4);n.writeUInt32BE(b.length);
  socket.end(Buffer.concat([n,b]));
}
async function snapshot(op){
  const states=await Promise.all(nodes.map(role));
  return op==='thread.nodes_up'
    ? states.filter(x=>['leader','router','child'].includes(x)).length
    : states.filter(x=>x==='leader').length;
}
function handle(socket){
  let buf=Buffer.alloc(0),complete=false;
  socket.setTimeout(2000,()=>socket.destroy());
  socket.on('data',async chunk=>{
    if(complete)return;
    buf=Buffer.concat([buf,chunk]);
    if(buf.length>260){complete=true;return socket.destroy();}
    if(buf.length<4)return;
    const n=buf.readUInt32BE(0);
    if(n===0||n>256){complete=true;return socket.destroy();}
    if(buf.length<n+4)return;
    complete=true;
    try {
      if(buf.length!==n+4)throw new Error('trailing data');
      const msg=decode(buf.subarray(4));
      if(!msg||typeof msg!=='object'||Array.isArray(msg) ||
         Object.keys(msg).length!==2||msg.v!==1 ||
         !['thread.nodes_up','thread.leaders'].includes(msg.op)) {
        reply(socket,{ok:false});return;
      }
      reply(socket,{ok:true,value:await snapshot(msg.op)});
    }catch{reply(socket,{ok:false});}
  });
  socket.on('error',()=>{});
}
function cleanup(){
  server?.close();
  for(const n of nodes)n.shutdown();
  try{if(existsSync(socketPath))unlinkSync(socketPath);}catch{}
}
process.once('SIGINT',()=>{cleanup();process.exit(0);});
process.once('SIGTERM',()=>{cleanup();process.exit(0);});
try{
  await init();
  server=net.createServer(handle);
  server.listen(socketPath,()=>{
    chmodSync(socketPath,0o600);
    console.log('THREAD_BRIDGE_READY '+socketPath);
  });
}catch(e){
  console.error('THREAD_BRIDGE_ERROR '+e.message);
  cleanup();process.exitCode=1;
}
