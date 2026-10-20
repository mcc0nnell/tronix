// Operator-run, single-user synthetic IHB campaign broker for Tronix.
// This is a LAB bridge, not a privileged service or an admitted sandbox.
import net from 'node:net';
import {encode, decode} from '@msgpack/msgpack';
import {spawn, execFileSync} from 'node:child_process';
import {randomUUID, createHash} from 'node:crypto';
import {mkdirSync, readFileSync, writeFileSync, chmodSync, lstatSync, rmSync, existsSync} from 'node:fs';
import {resolve, join} from 'node:path';

const socket = process.env.TRONIX_IHB_BROKER_SOCKET;
const ihb = process.env.TRONIX_IHB_ROOT;
const output = process.env.TRONIX_IHB_OUTPUT;
const image = process.env.TRONIX_IHB_IMAGE_ID;
const IMAGE = /^sha256:[a-f0-9]{64}$/;
if (!socket?.startsWith('/') || !ihb?.startsWith('/') || !output?.startsWith('/') ||
    !IMAGE.test(image ?? '')) throw Error('absolute socket, IHB root, output and pinned image ID required');
if (socket.length > 99) throw Error('socket path too long');
const root = resolve(ihb), storage = resolve(output);
if (existsSync(socket)) throw Error('socket already exists; refuse to replace');
if (!existsSync(join(root, 'run.py')) || !existsSync(join(root, 'isolated.py')) ||
    !existsSync(join(root, 'fixtures', 'buggy.c')) ||
    !existsSync(join(root, 'fixtures', 'safe.c'))) throw Error('missing reviewed IHB fixture files');
mkdirSync(storage, {recursive:true,mode:0o700});
const stat = lstatSync(storage);
if (!stat.isDirectory() || stat.isSymbolicLink() || stat.uid !== process.getuid() ||
    (stat.mode & 0o077) !== 0) throw Error('output must be private and owned');
const actual = execFileSync('docker', ['image','inspect','--format','{{.Id}}',image],
                           {encoding:'utf8',timeout:15000}).trim();
if (actual !== image) throw Error('local Docker image digest mismatch');
const seedDir = join(storage,'seeds');
mkdirSync(seedDir,{mode:0o700});
const seed = join(seedDir,'zero');
if (!existsSync(seed)) writeFileSync(seed,Buffer.from([0]),{flag:'wx',mode:0o600});
if (!readFileSync(seed).equals(Buffer.from([0]))) throw Error('seed mutated');

const profiles = Object.freeze({
  'fuzz.fixture.safe': {source:'safe.c',runs:96,expect:'no_failure_observed',code:1,exit:0},
  'fuzz.fixture.buggy': {source:'buggy.c',runs:1000,expect:'asan_failure',code:2,exit:1},
});
let busy=false;
function sha(b){return createHash('sha256').update(b).digest('hex');}
function run(argv){
  return new Promise((done)=>{
    const child=spawn('python3',argv,{cwd:root,env:{
      PATH:process.env.PATH ?? '/usr/bin:/bin',HOME:storage,LANG:'C'},
      stdio:['ignore','pipe','pipe']});
    let stdout='',stderr='';
    child.stdout.on('data',b=>{if(stdout.length<4096)stdout+=b.toString().slice(0,4096);});
    child.stderr.on('data',b=>{if(stderr.length<4096)stderr+=b.toString().slice(0,4096);});
    child.on('error',error=>done({exit:null,error:String(error),stdout,stderr}));
    child.on('close',(exit)=>done({exit,stdout,stderr}));
  });
}
async function execute(op){
  if(busy) return {ok:false,value:0};
  const profile=profiles[op];
  if(!profile) return {ok:false,value:0};
  busy=true;
  const id=randomUUID();
  const folder=join(storage,'campaign-'+id);
  try {
    const argv=[join(root,'isolated.py'),'--source',join(root,'fixtures',profile.source),
      '--corpus',seedDir,'--out',folder,'--image',image,
      '--memory-mb','1024','--runs',String(profile.runs),'--seconds','8','--seed','17'];
    const result=await run(argv);
    const file=join(folder,'campaign','evidence.json');
    if (!existsSync(file)) return {ok:false,value:0};
    const bytes=readFileSync(file);
    const evidence=JSON.parse(bytes.toString('utf8'));
    const pass=result.exit===profile.exit && evidence.status===profile.expect;
    const receipt={schema:'tronix.ihb.fixture-receipt.v0',campaign_id:id,profile:op,
       image_id:image,executor_exit:result.exit,ihb_status:evidence.status,
       evidence_sha256:sha(bytes),admission:'not_performed',
       validated_fixture_result:pass};
    writeFileSync(join(folder,'broker-receipt.json'),
      JSON.stringify(receipt,null,2)+'\n',{flag:'wx',mode:0o600});
    return {ok:pass,value:pass?profile.code:0};
  } catch(error) {
    console.error('fixture broker error:',String(error));
    return {ok:false,value:0};
  } finally {busy=false;}
}
function respond(client,result){
  const b=Buffer.from(encode(result));
  const header=Buffer.alloc(4);header.writeUInt32BE(b.length);
  client.end(Buffer.concat([header,b]));
}
const server=net.createServer((client)=>{
  let data=Buffer.alloc(0);
  client.on('data',async(part)=>{
    data=Buffer.concat([data,part]);
    if(data.length>512){respond(client,{ok:false,value:0});return;}
    if(data.length<4)return;
    const length=data.readUInt32BE(0);
    if(length<1 || length>256 || data.length>length+4){
      respond(client,{ok:false,value:0});return;
    }
    if(data.length!==length+4)return;
    client.pause();
    let request;
    try{request=decode(data.subarray(4));}
    catch{respond(client,{ok:false,value:0});return;}
    if(!request || typeof request!=='object' || Array.isArray(request) ||
       Object.keys(request).length!==2 || request.v!==1 ||
       typeof request.op!=='string' || !Object.hasOwn(profiles,request.op)){
      respond(client,{ok:false,value:0});return;
    }
    respond(client,await execute(request.op));
  });
  client.on('error',()=>{});
});
server.listen(socket,()=>{
  chmodSync(socket,0o600);
  console.log('TRONIX_IHB_BROKER_READY');
});
function shutdown(){
  server.close();
  try{rmSync(socket);}catch{}
}
process.on('SIGINT',shutdown);
process.on('SIGTERM',shutdown);
