import assert from 'node:assert/strict';
import {dirname,join,resolve} from 'node:path';
import {apiBase,databaseUrl} from './architecture-fixture';
const db=new Bun.SQL(databaseUrl);
try{
 const [location]=await db`SELECT current_setting('data_directory') AS path`;
 assert.match(resolve(location.path).replaceAll('\\','/'),/\/build\/system-orm-fixture-[a-f0-9]+\/postgres$/i);
 const directory=join(dirname(location.path),'firmware');
 const end=Date.now()+15000;
 while((await fetch(apiBase+'/internal/health/ready')).status!==200){if(Date.now()>end)throw Error('workers not ready');await Bun.sleep(50);}
 assert.equal(await Bun.file(join(directory,'00000000-0000-4000-8000-000000000004.upload')).exists(),false);
 assert.equal((await Bun.file(join(directory,'00000000-0000-4000-8000-000000000005.bin')).text()).trim(),'preserved');
 assert.equal((await Bun.file(join(directory,'unrelated.upload')).text()).trim(),'unrelated');
 console.log('PASS actual Service Worker startup reclaims abandoned upload and preserves registered and unrelated files');
}finally{await db.close();}
