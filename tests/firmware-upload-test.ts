import { afterAll, afterEach, expect, test, spyOn } from 'bun:test';
import request from '../web/lib/http';
import { upgradeFirmware } from '../web/pages/iot/edge_node/edge_node.service';
const send=spyOn(request,'post');
afterEach(()=>send.mockReset());afterAll(()=>send.mockRestore());
const node='00000000-0000-7000-8000-000000000001';
test('new firmware checks its hash then uploads once with progress', async()=>{
 const file=new File([new Uint8Array([0,255,13,10])],'test.bin'); const progress:number[]=[];
 send.mockImplementation(async(path,body,config)=>{
  if(path.endsWith('/reuse')) {
   expect(body).toEqual({sha256: new Bun.CryptoHasher('sha256').update(new Uint8Array([0,255,13,10])).digest('hex'),sizeBytes:4,keepSettings:true});
   return {reused:false};
  }
  expect(path).toBe(`/v1/edge/${node}/firmware`);expect(body).toBe(file);
  expect(config?.params).toEqual({fileName:'test.bin',sizeBytes:4,keepSettings:true});
  expect(config?.headers).toEqual({'Content-Type':'application/octet-stream'});
  config?.onUploadProgress?.(2,4);config?.onUploadProgress?.(4,4);
 });
 await upgradeFirmware(node,{file,keepSettings:true},value=>progress.push(value.percent));
 expect(send).toHaveBeenCalledTimes(2);expect(progress).toEqual([50,100]);
});
test('existing content is distributed without uploading the file again',async()=>{
 const progress:number[]=[];
 send.mockResolvedValue({reused:true});
 await upgradeFirmware(node,{file:new File(['abc'],'renamed.bin'),keepSettings:false},value=>progress.push(value.percent));
 expect(send).toHaveBeenCalledTimes(1);
 expect(send.mock.calls[0][0]).toBe(`/v1/edge/${node}/firmware/reuse`);
 expect(send.mock.calls[0][1]).toEqual({sha256:'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',sizeBytes:3,keepSettings:false});
 expect(progress).toEqual([100]);
});
test('an uncertain upload failure is not replayed automatically',async()=>{
 send.mockImplementation(async(path)=>{if(path.endsWith('/reuse'))return {reused:false};throw new Error('response lost');});
 await expect(upgradeFirmware(node,{file:new File(['abc'],'test.bin'),keepSettings:false})).rejects.toThrow('response lost');
 expect(send).toHaveBeenCalledTimes(2);
});
test('an uncertain reuse failure does not dispatch a second upgrade',async()=>{
 send.mockRejectedValue(new Error('response lost'));
 await expect(upgradeFirmware(node,{file:new File(['abc'],'test.bin'),keepSettings:false})).rejects.toThrow('response lost');
 expect(send).toHaveBeenCalledTimes(1);
});
