// SPDX-License-Identifier: Apache-2.0
// Owner-only, fixed-scope local Rizzer harness execution bridge.
// TRONIX v2 remains the admission/scheduling authority. The sidecar accepts
// no caller-selected path, target, shell command, scan family or network URL.
import assert from 'node:assert/strict';
import {createHash, timingSafeEqual} from 'node:crypto';
import {spawn} from 'node:child_process';
import {open,readFile,stat,lstat,mkdir,chmod} from 'node:fs/promises';
import {createServer} from 'node:net';
import {dirname,isAbsolute,resolve,join} from 'node:path';
import {encode,decode} from '@msgpack/msgpack';

const SHA=/^[0-9a-f]{64}$/;
const OPTIONS=new Set([
  '--socket','--token-file','--rizzer-cli','--rizzer-cli-sha256',
  '--catalog','--catalog-sha256','--host','--host-sha256','--bundle',
  '--bundle-sha256','--evidence-dir'
]);
function argsFrom(argv) {
  if(argv.length!==OPTIONS.size*2)throw Error('all exact options required');
  const options=new Map();
  for(let i=0;i<argv.length;i+=2){
    if(!OPTIONS.has(argv[i])||options.has(argv[i])||!argv[i+1])throw Error('unknown/repeated option');
    options.set(argv[i],argv[i+1]);
  }
  for(const key of OPTIONS)if(!options.has(key))throw Error('missing option '+key);
  for(const key of OPTIONS)if(!key.includes('sha256')&&!isAbsolute(options.get(key)))
    throw Error('absolute paths required');
  for(const key of ['--rizzer-cli-sha256','--host-sha256','--bundle-sha256','--catalog-sha256'])
    if(!SHA.test(options.get(key)))throw Error('invalid artifact SHA pin');
  return options;
}
const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
async function regular(path,maximum,{privateFile=false}={}) {
  const pathStat=await lstat(path);
  if(!pathStat.isFile()||pathStat.isSymbolicLink()||pathStat.size<=0||
     pathStat.size>maximum||pathStat.uid!==process.getuid()||
     (privateFile&&(pathStat.mode&0o077)!==0))throw Error('untrusted file: '+path);
  let cursor='/';
  for(const part of resolve(path).split('/').filter(Boolean)){
    if(part==='.'||part==='..')throw Error('path traversal');
    cursor=join(cursor,part);
    const st=await lstat(cursor);
    if(st.isSymbolicLink())throw Error('symlink path');
  }
  return readFile(path);
}
async function privateDir(path) {
  const st=await lstat(path);
  if(!st.isDirectory()||st.isSymbolicLink()||st.uid!==process.getuid()||
     (st.mode&0o077)!==0)throw Error('directory not owner-private: '+path);
}
function strictObject(obj,keys){
  if(!obj||Array.isArray(obj)||typeof obj!=='object'||
     Object.keys(obj).length!==keys.length ||
     keys.some(k=>!Object.hasOwn(obj,k)))throw Error('unexpected MsgPack request');
}
const reply=(ok,value,digest)=>{
  const payload=ok?{ok:true,value,digest}:{ok:false,value:0};
  const body=Buffer.from(encode(payload));
  const frame=Buffer.allocUnsafe(4+body.length);
  frame.writeUInt32BE(body.length,0);
  body.copy(frame,4);
  return frame;
};
async function verifyConfig(o) {
  const socket=o.get('--socket'), evidence=o.get('--evidence-dir');
  const bridgeRoot='/tmp/tronix-rizzer-harness-'+process.getuid();
  if(dirname(socket)!==bridgeRoot ||
     o.get('--token-file')!==join(bridgeRoot,'bridge.token') ||
     socket!==join(bridgeRoot,'harness.sock'))
    throw Error('socket and token must follow the reviewed UID-scoped worker convention');
  await privateDir(dirname(socket));
  await privateDir(evidence);
  const key=(await regular(o.get('--token-file'),128,{privateFile:true})).toString();
  if(!SHA.test(key))throw Error('token file must be exactly 64 lowercase hex chars');
  for(const [pathArg,shaArg] of [['--rizzer-cli','--rizzer-cli-sha256'],
                                   ['--host','--host-sha256'],
                                   ['--bundle','--bundle-sha256']]){
    const bytes=await regular(o.get(pathArg),4*1024*1024);
    if(hash(bytes)!==o.get(shaArg))throw Error('operator artifact pin mismatch '+pathArg);
  }
  const catalogPath=o.get('--catalog');
  if(catalogPath!==join(bridgeRoot,'catalog.json'))
    throw Error('catalog path must be fixed, owner-private worker path');
  const pin=(await regular(join(bridgeRoot,'catalog.sha256'),64,{privateFile:true})).toString();
  const raw=await regular(catalogPath,8192,{privateFile:true});
  if(!SHA.test(pin)||pin!==o.get('--catalog-sha256')||hash(raw)!==pin)
    throw Error('catalog source digest mismatch');
  const doc=JSON.parse(raw);
  strictObject(doc,['schema','entries']);
  if(doc.schema!=='tronix.rizzer.catalog.v1'||!Array.isArray(doc.entries)||
     doc.entries.length<1||doc.entries.length>8)throw Error('invalid catalog version/count');
  const entries=new Map();
  const families=new Set(['mcp-authorization','sixlowpan-fragments',
                          'spinel-packed-uint','evm-fullmath']);
  for(const item of doc.entries){
    strictObject(item,['operation','family','casebook','ir_sha256']);
    if(typeof item.operation!=='string'||
       !/^rizzer\.harness\.[a-z0-9_-]{1,33}$/.test(item.operation)||
       item.operation.length>48||entries.has(item.operation)||
       !families.has(item.family)||!isAbsolute(item.casebook)||
       typeof item.ir_sha256!=='string'||!SHA.test(item.ir_sha256))
      throw Error('invalid or duplicate catalog cartridge');
    await privateDir(item.casebook);
    for(const name of ['manifest.json','ir.json','build-receipt.json'])
      await regular(join(item.casebook,name),262144);
    const irRaw=await regular(join(item.casebook,'ir.json'),262144);
    if(hash(irRaw)!==item.ir_sha256)
      throw Error('catalog IR source pin differs');
    const ir=JSON.parse(irRaw);
    if(ir.schema!=='rizzer.harness-ir.v1'||ir.family!==item.family||
       ir.execution!=='offline_reference_only')
      throw Error('catalog IR authority/family mismatch');
    entries.set(item.operation,item);
  }
  const cli=o.get('--rizzer-cli');
  if(!cli.endsWith('/rizzer_cli.py'))throw Error('expected reviewed Rizzer CLI path');
  return {socket,evidence,key,cli,catalog:entries,catalogSha:pin,
          host:o.get('--host'),hostSha:o.get('--host-sha256'),
          bundle:o.get('--bundle'),bundleSha:o.get('--bundle-sha256')};
}
async function inspectReceipt(directory) {
  const receiptRaw=await regular(join(directory,'receipt.json'),65536,{privateFile:true});
  const reportRaw=await regular(join(directory,'native-parity.json'),65536,{privateFile:true});
  const receipt=JSON.parse(receiptRaw),report=JSON.parse(reportRaw);
  if(receipt.schema!=='ihb.cartridge-receipt.v1'||receipt.status!=='done'||
     report.schema!=='rizzer.celix-harness-parity.v1'||
     report.status!=='reference_parity'||report.target_executed!==false||
     report.new_vulnerability_confirmed!==false||
     report.mismatches?.length!==0||report.independent_production_reference!==false||
     !Number.isInteger(report.parity_matches)||report.parity_matches<3||
     report.parity_matches>32||receipt.outputs?.[0]?.native_case_count!==report.parity_matches)
    throw Error('Rizzer native parity receipt is not a verified offline reference result');
  if(receipt.artifacts?.[0]?.sha256!=='sha256:'+hash(reportRaw))
    throw Error('Rizzer receipt evidence digest mismatch');
  return {count:report.parity_matches,receipt_sha256:hash(receiptRaw),
          report_sha256:hash(reportRaw),ir_sha256:report.ir_sha256};
}
async function spawnRizzer(config,entry,out) {
  const command='/usr/bin/python3';
  const argv=['-B',config.cli,'scan','harness-native',
              '--casebook',entry.casebook,
              '--host',config.host,'--host-sha256',config.hostSha,
              '--celix-bundle',config.bundle,
              '--celix-bundle-sha256',config.bundleSha,
              '--out',out];
  return await new Promise((finish,reject)=>{
    const proc=spawn(command,argv,{shell:false,stdio:['ignore','pipe','pipe'],
      env:{PATH:'/usr/bin:/bin',HOME:dirname(config.socket),
           TMPDIR:dirname(config.socket),LANG:'C',LC_ALL:'C',
           PYTHONDONTWRITEBYTECODE:'1'},cwd:dirname(config.cli)});
    let stdout='',stderr='',done=false;
    const timer=setTimeout(()=>{proc.kill('SIGKILL');},7500);
    proc.stdout.on('data',buf=>{stdout+=(buf.toString()).slice(0,4096);
      if(stdout.length>8192)proc.kill('SIGKILL');});
    proc.stderr.on('data',buf=>{stderr+=(buf.toString()).slice(0,2048);
      if(stderr.length>8192)proc.kill('SIGKILL');});
    proc.on('error',err=>{if(done)return;done=true;clearTimeout(timer);reject(err)});
    proc.on('close',(code,signal)=>{if(done)return;done=true;clearTimeout(timer);
      if(code!==0||signal)reject(Error('fixed Rizzer bridge failed (no target authority)'));
      else finish({stdout});});
  });
}
async function main() {
  const c=await verifyConfig(argsFrom(process.argv.slice(2)));
  let attempts=0,active=false;
  const server=createServer(socket=>{
    let buffer=Buffer.alloc(0),seen=false;
    socket.setTimeout(9000,()=>socket.destroy());
    socket.on('error',()=>{});
    socket.on('data',async chunk=>{
      if(seen)return;
      buffer=Buffer.concat([buffer,chunk]);
      if(buffer.length>2100){seen=true;socket.end(reply(false,0));return;}
      if(buffer.length<4)return;
      const n=buffer.readUInt32BE(0);
      if(n<1||n>2048){seen=true;socket.end(reply(false,0));return;}
      if(buffer.length<4+n)return;
      seen=true;
      try{
        const body=decode(buffer.subarray(4,4+n));
        strictObject(body,['v','op','token']);
        const received=Buffer.from(String(body.token));
        const expected=Buffer.from(c.key);
        if(body.v!==1||typeof body.op!=='string'||
           received.length!==expected.length||
           !timingSafeEqual(received,expected)||!c.catalog.has(body.op)||
           active||attempts>=8)
          throw Error('unauthorized/busy request');
        active=true;
        attempts++;
        const name='run-'+String(attempts).padStart(4,'0');
        const folder=join(c.evidence,name);
        try{
          const selected=c.catalog.get(body.op);
          await spawnRizzer(c,selected,folder);
          const obs=await inspectReceipt(folder);
          const report=JSON.parse(await regular(join(folder,'native-parity.json'),65536));
          if(report.family!==selected.family)throw Error('wrong corpus family produced');
          const audit={schema:'tronix.rizzer.sidecar.v2',run:name,
                       operation:body.op,catalog_sha256:c.catalogSha,offline_only:true,
                       target_executed:false,receipt:obs,
                       cli_sha256:hash(await regular(c.cli,4*1024*1024)),
                       bundle_sha256:c.bundleSha,host_sha256:c.hostSha};
          const auditPath=join(c.evidence,name+'.dispatch.json');
          const fd=await open(auditPath,'wx',0o600);
          try{await fd.writeFile(JSON.stringify(audit)+'\n');await fd.sync()}
          finally{await fd.close()}
          socket.end(reply(true,obs.count,obs.report_sha256));
        }finally{active=false}
      }catch(err){
        active=false;
        console.error('RIZZER_HARNESS_BRIDGE_REQUEST_REFUSED:',err.message);
        socket.end(reply(false,0));
      }
    });
  });
  await new Promise((ok,fail)=>{server.once('error',fail);server.listen(c.socket,ok)});
  await chmod(c.socket,0o600);
  console.log('RIZZER_HARNESS_BRIDGE_READY');
  process.on('SIGTERM',()=>{server.close();});
  process.on('SIGINT',()=>{server.close();});
}
main().catch(err=>{console.error('Rizzer harness bridge refused startup:',err.message);process.exitCode=78;});
