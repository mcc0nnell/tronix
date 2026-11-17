// SPDX-License-Identifier: Apache-2.0
// Evidence-only selective Atlas Watcher handoff. This does not invoke Watcher.
import {createHash} from 'node:crypto';
import {open,writeFile,mkdir,rm,lstat} from 'node:fs/promises';
import {constants as fsConstants} from 'node:fs';
import path from 'node:path';
import {verifyRequest} from './verify_client.mjs';
const sha=b=>createHash('sha256').update(b).digest('hex');
export async function prepareReviewHandoff(socket,taskId,answer,outDir){
  if(!(answer instanceof Uint8Array)||answer.length>65536)
    throw Error('bounded binary answer required');
  const localHash=sha(answer);
  const receipt=await verifyRequest(socket,taskId,answer);
  if(receipt.ok!==true||receipt.protocol!=='tronix.typed-verification/v2'||
     receipt.taskId!==taskId||receipt.actualSha256!==localHash||
     !['VERIFIED','REJECTED','UNVERIFIABLE'].includes(receipt.status)||
     receipt.executionAuthorized!==false||receipt.published!==false)
    throw Error('invalid or unbound native verifier result');
  const uncertain=receipt.status==='UNVERIFIABLE' &&
    receipt.route==='atlas-review-required' &&
    receipt.humanReview==='pending' &&
    receipt.objectiveTruthEstablished===false;
  if(!uncertain){
    if(receipt.route!=='none')throw Error('contradictory verifier route');
    return {created:false,status:receipt.status,reason:'no escalation warranted'};
  }
  if(typeof outDir!=='string'||!path.isAbsolute(outDir)||path.normalize(outDir)!==outDir)
    throw Error('absolute canonical handoff path required');
  // The evidence destination must be inside an existing owner-private
  // directory, never under an uncontrolled or symlinked parent.
  const parentPath=path.dirname(outDir);
  let ancestor=path.parse(parentPath).root;
  for(const part of parentPath.slice(ancestor.length).split(path.sep).filter(Boolean)){
    ancestor=path.join(ancestor,part);
    if((await lstat(ancestor)).isSymbolicLink())throw Error('symlink in handoff path');
  }
  const parent=await lstat(parentPath);
  if(!parent.isDirectory()||parent.isSymbolicLink()||
     parent.uid!==process.getuid()||(parent.mode&0o077)!==0)
    throw Error('handoff parent must be owner-private');
  // Fail closed instead of overwriting any existing job or receipt.
  await mkdir(outDir,{mode:0o700});
  try{
    const handoff={
      schema:'tronix.atlas-review-handoff/v1',
      kind:'unverified-answer-review-request',
      taskId,
      status:'UNVERIFIABLE',
      routerProtocol:receipt.protocol,
      oracleManifestSha256:receipt.oracleManifestSha256,
      answerSha256:localHash,
      answerBytes:answer.length,
      reason:'no independently precommitted golden oracle for this task',
      humanReview:'pending',
      executionAuthorized:false,
      published:false,
      note:'Inert evidence only. Not a Rizzer atlas-operator-job. No model, shell, SCM, or publication authority.'
    };
    await writeFile(path.join(outDir,'answer.bin'),answer,{flag:'wx',mode:0o600});
    await writeFile(path.join(outDir,'handoff.json'),
      JSON.stringify(handoff,null,2)+'\n',{flag:'wx',mode:0o600});
    return {created:true,status:'UNVERIFIABLE',path:outDir,answerSha256:localHash};
  }catch(e){
    await rm(outDir,{recursive:true,force:true});
    throw e;
  }
}
async function readInput(file){
  if(typeof file!=='string'||!path.isAbsolute(file)||path.normalize(file)!==file)
    throw Error('canonical absolute input file required');
  let ancestor=path.parse(file).root;
  for(const part of file.slice(ancestor.length).split(path.sep).filter(Boolean)){
    ancestor=path.join(ancestor,part);
    if((await lstat(ancestor)).isSymbolicLink())throw Error('symlink input path');
  }
  const handle=await open(file,fsConstants.O_RDONLY|fsConstants.O_NOFOLLOW|fsConstants.O_NONBLOCK);
  try{
    const before=await handle.stat();
    if(!before.isFile()||before.size>65536)throw Error('invalid input file');
    const bytes=await handle.readFile();
    const after=await handle.stat();
    if(bytes.length>65536||before.size!==after.size||
       before.mtimeMs!==after.mtimeMs||bytes.length!==before.size)
      throw Error('answer changed while read');
    return bytes;
  }finally{await handle.close();}
}
if(process.argv[1]&&path.resolve(process.argv[1])===path.resolve(new URL(import.meta.url).pathname)){
  const entries=process.argv.slice(2);
  if(entries.length!==8)throw Error('usage: --socket PATH --task TASK --input FILE --out DIRECTORY');
  const opts=new Map();
  for(let i=0;i<entries.length;i+=2){
    if(!['--socket','--task','--input','--out'].includes(entries[i])||opts.has(entries[i]))
      throw Error('unknown or duplicate argument');
    opts.set(entries[i],entries[i+1]);
  }
  if(opts.size!==4)throw Error('all options required');
  const result=await prepareReviewHandoff(opts.get('--socket'),opts.get('--task'),
    await readInput(opts.get('--input')),opts.get('--out'));
  console.log(JSON.stringify(result));
}
