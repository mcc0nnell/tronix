// Copyright 2026 Robert McConnell
// SPDX-License-Identifier: Apache-2.0
// Native-independent fixture: HMAC-chained mixed v1/v2 grant receipts bind
// complete Rizzer report hashes, including rejection of modified report bytes.
import assert from 'node:assert/strict';
import {createHash,createHmac,randomBytes} from 'node:crypto';
import {spawnSync} from 'node:child_process';
import {mkdtemp,mkdir,writeFile,readFile,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';

const repo=resolve(import.meta.dirname,'..');
const root=await mkdtemp(join(tmpdir(),'tronix-signed-v2-'));
const sha=bytes=>createHash('sha256').update(bytes).digest('hex');
const hex=s=>Buffer.from(s).toString('hex');
const checks=[];
const check=(what,condition)=>{assert.ok(condition,what);checks.push(what)};
const adapter=join(repo,'build/tronix_demo.zip');
const key=randomBytes(32);
const keyFile=join(root,'key');
const grants=join(root,'grants.conf');
const ledger=join(root,'evidence.txt');
const artifactRoot=join(root,'artifacts');
await mkdir(artifactRoot,{mode:0o700});
const run=join(artifactRoot,'run-0001');
await mkdir(run,{mode:0o700});
const report=join(run,'native-parity.json');
const reportBytes=Buffer.from('{"schema":"rizzer.celix-harness-parity.v1","target_executed":false}\n');
let legacyMac='';
function record(header){
  const tag=createHmac('sha256',key).update(header).digest('hex');
  return [header,tag].join(' ')+'\n';
}
function verify(extra=[]){
  return spawnSync('python3',['scripts/verify_grant_evidence.py',
    '--key',keyFile,'--grants',grants,'--adapter',adapter,
    '--evidence',ledger,...extra],{cwd:repo,encoding:'utf8',timeout:10000});
}
try{
  const adapterBytes=await readFile(adapter);
  const adapterSha=sha(adapterBytes);
  const grantRaw=[
    'tronix.grants.v1','bundle org.tronix.adapter.demo','version 0.1.0',
    'artifact '+adapterSha,'allow demo.constant',''
  ].join('\n');
  const grantSha=sha(Buffer.from(grantRaw));
  await writeFile(keyFile,key,{mode:0o600});
  await writeFile(grants,grantRaw,{mode:0o600});
  await writeFile(report,reportBytes,{mode:0o600});
  const op=hex('demo.constant');
  const legacyHeader=[
    'v1','1',hex('legacy'),hex('fixture'),op,'completed','1','42',
    adapterSha,grantSha,'0'.repeat(64)
  ].join(' ');
  const legacy=record(legacyHeader);
  legacyMac=legacy.trimEnd().split(' ').at(-1);
  const digest=sha(reportBytes);
  const v2Header=[
    'v2','2',hex('native-run'),hex('fixture'),op,'completed','2','42',
    digest,adapterSha,grantSha,legacyMac
  ].join(' ');
  const v2=record(v2Header);
  await writeFile(ledger,legacy+v2,{mode:0o600});
  const signed=verify();
  check('mixed v1/v2 HMAC chain verifies without artifact files',
    signed.status===0&&JSON.parse(signed.stdout).verified===2);
  check('signed report digest is exposed by the independent verifier',
    JSON.parse(signed.stdout).artifactReferences?.[0]?.reportSha256===digest);
  const bound=verify(['--artifact-dir',artifactRoot]);
  check('full SHA-256 of actual Rizzer artifact is bound to signed v2 receipt',
    bound.status===0&&JSON.parse(bound.stdout).artifactFilesVerified===true);
  await writeFile(report,Buffer.concat([reportBytes,Buffer.from(' ')]),{mode:0o600});
  check('altered artifact rejected even when original HMAC chain remains valid',
    verify(['--artifact-dir',artifactRoot]).status!==0);
  await writeFile(report,reportBytes,{mode:0o600});
  const edited=v2Header.split(' ');
  edited[8]=(digest[0]==='a'?'b':'a')+digest.slice(1);
  await writeFile(ledger,legacy+edited.join(' ')+' '+v2.trimEnd().split(' ').at(-1)+'\n',{mode:0o600});
  check('altered signed v2 digest fails HMAC verification',verify().status!==0);
  await writeFile(ledger,legacy+record(edited.join(' ')),{mode:0o600});
  check('an adversary with daemon HMAC key can authenticate a different digest',
    verify().status===0);
  check('independent artifact comparison rejects a self-consistent forged digest',
    verify(['--artifact-dir',artifactRoot]).status!==0);
  await writeFile(ledger,legacy+v2,{mode:0o600});
  check('unmodified evidence again verifies',verify(['--artifact-dir',artifactRoot]).status===0);
  console.log('TRONIX_SIGNED_ARTIFACT_V2_TEST_PASS',JSON.stringify({checks:checks.length,authenticatedDigest:digest}));
} finally{
  await rm(root,{recursive:true,force:true});
}
