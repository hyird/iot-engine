import assert from 'node:assert/strict';
import {mkdir,writeFile,readFile,unlink,rmdir} from 'node:fs/promises';
import {resolve,join} from 'node:path';
const executable=resolve(process.argv[2]??'build/Release/api-upload-test.exe');
const directory=resolve('build','upload-recovery-fixture-'+crypto.randomUUID());
await mkdir(directory);
const orphan=join(directory,'00000000-0000-4000-8000-000000000004.upload');
const active=join(directory,'00000000-0000-4000-8000-000000000003.upload');
const registered=join(directory,'00000000-0000-4000-8000-000000000005.bin');
let owner:ReturnType<typeof Bun.spawn>|undefined;
try{
 owner=Bun.spawn([executable,'--hold',directory],{stdout:'pipe',stderr:'pipe'});
 const reader=owner.stdout.getReader();
 const ready=await Promise.race([reader.read(),Bun.sleep(10000).then(()=>{throw Error('owner did not start');})]);
 assert.match(new TextDecoder().decode(ready.value),/READY/);reader.releaseLock();
 await writeFile(orphan,'interrupted');await writeFile(registered,'registered');
 const recover=async()=>{const child=Bun.spawn([executable,'--recover',directory],{stdout:'pipe',stderr:'pipe'});const [status,out,err]=await Promise.all([child.exited,new Response(child.stdout).text(),new Response(child.stderr).text()]);assert.equal(status,0,err);return Number(out.trim());};
 assert.equal(await recover(),1,'live process upload must remain locked');
 assert.equal(await Bun.file(active).exists(),true);
 owner.kill('SIGKILL');await owner.exited;
 assert.equal(await recover(),1,'crashed owner upload must be reclaimed');
 assert.equal(await recover(),0,'recovery must be idempotent');
 assert.equal(await readFile(registered,'utf8'),'registered');
 console.log('PASS cross-process upload locking, forced termination recovery, idempotence, registered image preservation');
}finally{
 if(owner && owner.exitCode===null){owner.kill('SIGKILL');await owner.exited;}
 for(const file of [orphan,active,registered,join(directory,'.upload-recovery.lock')])await unlink(file).catch(error=>{if(error.code!=='ENOENT')throw error;});
 await rmdir(directory);
}
