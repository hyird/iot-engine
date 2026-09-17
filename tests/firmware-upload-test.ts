import { afterAll, afterEach, expect, test, spyOn } from 'bun:test';
import request from '../web/lib/http';
import { upgradeFirmware } from '../web/pages/iot/edge_node/edge_node.api';
const send=spyOn(request,'post');
afterEach(()=>send.mockReset());afterAll(()=>send.mockRestore());
const node='00000000-0000-7000-8000-000000000001';
test('firmware uses one raw HTTP request and preserves upload progress', async()=>{
 const file=new File([new Uint8Array([0,255,13,10])],'test.bin'); const progress:number[]=[];
 send.mockImplementation(async(path,body,config)=>{
  expect(path).toBe(`/v1/edge/${node}/firmware`);expect(body).toBe(file);
  expect(config?.params).toEqual({fileName:'test.bin',sizeBytes:4,keepSettings:true});
  expect(config?.headers).toEqual({'Content-Type':'application/octet-stream'});
  config?.onUploadProgress?.(2,4);config?.onUploadProgress?.(4,4);
 });
 await upgradeFirmware(node,{file,keepSettings:true},value=>progress.push(value.percent));
 expect(send).toHaveBeenCalledTimes(1);expect(progress).toEqual([50,100]);
});
test('an uncertain upload failure is not replayed automatically',async()=>{
 send.mockRejectedValue(new Error('response lost'));
 await expect(upgradeFirmware(node,{file:new File(['abc'],'test.bin'),keepSettings:false})).rejects.toThrow('response lost');
 expect(send).toHaveBeenCalledTimes(1);
});
