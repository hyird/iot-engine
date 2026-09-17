import assert from 'node:assert/strict';
import {resolve} from 'node:path';
const executable=resolve(process.argv[2]??'build/windows-client-cmake/Release/vpn_platform_http_probe.exe');
const peer='00000000-0000-4000-8000-000000000002';
const paths:string[]=[];
const streams=new Set<ReadableStreamDefaultController<Uint8Array>>();
const encoder=new TextEncoder();
const snapshot=(revision:number)=>encoder.encode(`event: snapshot\ndata: ${JSON.stringify({code:0,data:{revision,payload:'中'.repeat(40000)}})}\n\n`);
let failure:unknown;
const server=Bun.serve({hostname:'127.0.0.1',port:0,
 async fetch(request){try{
    const path=new URL(request.url).pathname; paths.push(request.method+' '+path);
    const respond=(data:unknown)=>Response.json({code:0,message:'ok',data});
    if(path==='/v1/auth/login') {assert.equal(request.method,'POST');assert.equal((await request.json()).username,'fixture-user');return respond({token:'token-one'});}
    if(path==='/v1/auth/refresh') {assert.equal(request.method,'POST');assert.equal((await request.json()).refresh_token,'refresh-one');return respond({token:'token-two'});}
    assert.equal(request.headers.get('Authorization'),'Bearer token-two');
    if(path==='/v1/vpn/desktop/devices') {assert.equal(request.method,'GET');return respond([{id:'device'}]);}
    if(path==='/v1/vpn/desktop/peers') {assert.equal(request.method,'POST');return respond({id:peer});}
    if(path===`/v1/vpn/desktop/peers/${peer}/config`) {assert.equal(request.method,'GET');return respond({revision:1});}
    if(path===`/v1/vpn/desktop/peers/${peer}/config/events`) {
        assert.equal(request.method,'GET');assert.equal(request.headers.get('Accept'),'text/event-stream');
        let controller:ReadableStreamDefaultController<Uint8Array>;
        return new Response(new ReadableStream<Uint8Array>({start(value){controller=value;streams.add(value);const initial=snapshot(1);value.enqueue(initial.slice(0,71));value.enqueue(initial.slice(71));value.enqueue(encoder.encode('event: heartbeat\ndata: {}\n\n'));},cancel(){streams.delete(controller);}}),{headers:{'Content-Type':'text/event-stream'}});
    }
    if(path===`/v1/vpn/desktop/peers/${peer}` && request.method==='PATCH') {
        assert.deepEqual(await request.json(),{});for(const stream of streams)stream.enqueue(snapshot(2));return respond({id:peer});
    }
    if(path===`/v1/vpn/desktop/peers/${peer}` && request.method==='DELETE')return respond(null);
    throw Error('unexpected HTTP request '+request.method+' '+path);
 }catch(error){failure=error;return Response.json({code:10004,message:'fixture failed'},{status:500});}}
});
try{
 const child=Bun.spawn([executable,String(server.port),'api'],{stdout:'pipe',stderr:'pipe'});
 const timer=setTimeout(()=>child.kill(),15000);
 const [status,stdout,stderr]=await Promise.all([child.exited,new Response(child.stdout).text(),new Response(child.stderr).text()]);clearTimeout(timer);
 if(failure)throw failure;
 assert.equal(status,0,stderr||stdout||'native API timed out');
 assert.equal(paths.filter(p=>p.endsWith('/config/events')).length,2);
 assert.equal(paths.filter(p=>p.endsWith('/desktop/devices')).length,3);
 assert(!paths.some(p=>p.includes('/channel')));
 console.log(stdout.trim());
}finally{server.stop(true);}
