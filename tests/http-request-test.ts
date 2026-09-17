import { describe, expect, test } from 'bun:test';
import { HttpClient, HttpRequestError, queryUrl, uploadWithProgress } from '../web/lib/http';

const json = (data: unknown, status = 200) => new Response(JSON.stringify(data), { status });

describe('HTTP business requests', () => {
    test('uses HTTP methods, encoded query parameters and bearer authentication', async () => {
        const received: Array<{url: string; method: string; authorization: string | null; body?: BodyInit | null; keepalive?: boolean}> = [];
        const client = new HttpClient({
            token: () => 'test-token', refresh: async () => false,
            fetch: (async (input, init) => {
                received.push({url: String(input), method: init?.method ?? 'GET', authorization: new Headers(init?.headers).get('Authorization'), body: init?.body, keepalive: init?.keepalive});
                return json({code: 0, message: 'ok', data: {saved: true}});
            }) as typeof fetch,
        });
        expect(await client.get('/v1/roles', {params: {keyword: 'A&B', page: 2}})).toEqual({saved: true});
        await client.post('/v1/roles', {name: 'test'});
        await client.put('/v1/roles/id', {name: 'changed'});
        await client.patch('/v1/roles/id', {status: 'disabled'});
        await client.delete('/v1/roles/id');
        expect(received.map(item => item.method)).toEqual(['GET','POST','PUT','PATCH','DELETE']);
        expect(received[0].url).toBe('/v1/roles?keyword=A%26B&page=2');
        expect(received.every(item => item.authorization === 'Bearer test-token')).toBe(true);
        expect(received[1].body).toBe('{"name":"test"}');
        await client.delete('/v1/alert/rules', {data: {ids: ['rule-a','rule-b']}});
        expect(received[5].method).toBe('DELETE');
        expect(received[5].body).toBe('{"ids":["rule-a","rule-b"]}');
        await client.post('/v1/gb28181/previews/session/stop', undefined, {keepalive:true});
        expect(received[6].keepalive).toBe(true);
        expect(received[6].authorization).toBe('Bearer test-token');
        expect(queryUrl('/v1/items?enabled=true', {empty: null, id: ['a','b']})).toBe('/v1/items?enabled=true&id=a&id=b');
        // An empty parent_id selects root departments; omitting it selects all departments.
        expect(queryUrl('/v1/departments', {parent_id: '', keyword: undefined})).toBe('/v1/departments?parent_id=');
    });

    test('refreshes an expired read once and never replays a write', async () => {
        let token = 'expired', calls = 0, refreshes = 0;
        const client = new HttpClient({
            token: () => token,
            refresh: async () => { refreshes++; token = 'fresh'; return true; },
            fetch: (async (_input, init) => {
                calls++;
                return new Headers(init?.headers).get('Authorization') === 'Bearer fresh'
                    ? json({code: 0, data: [1]}) : json({code: 11005, message: 'expired'}, 401);
            }) as typeof fetch,
        });
        expect(await client.get('/v1/roles')).toEqual([1]);
        expect([calls,refreshes]).toEqual([2,1]);
        token = 'expired';
        await expect(client.post('/v1/roles', {})).rejects.toBeInstanceOf(HttpRequestError);
        expect([calls,refreshes]).toEqual([3,1]);
    });

    test('does not send an aborted query or retry a server failure', async () => {
        let calls = 0;
        const client = new HttpClient({ token: () => null, refresh: async () => false,
            fetch: (async () => { calls++; return json({code: 10004, message: 'failed'}, 500); }) as typeof fetch });
        const controller = new AbortController(); controller.abort();
        await expect(client.get('/v1/roles', {signal: controller.signal})).rejects.toThrow();
        expect(calls).toBe(0);
        await expect(client.get('/v1/roles')).rejects.toThrow('failed');
        expect(calls).toBe(1);
    });
});


describe('HTTP upload transport', () => {
    function fixture() {
        const sent: {method?:string;url?:string;body?:unknown;headers:Record<string,string>}={headers:{}};
        const xhr={status:200,responseText:'{"code":0,"data":{"saved":true}}',upload:{onprogress:null as ((event:ProgressEvent)=>void)|null},
            onload:null as (()=>void)|null,onerror:null as (()=>void)|null,onabort:null as (()=>void)|null,
            open(method:string,url:string){sent.method=method;sent.url=url;},
            setRequestHeader(name:string,value:string){sent.headers[name]=value;},
            getResponseHeader(){return 'application/json';},send(body:unknown){sent.body=body;},
            abort(){this.onabort?.();}};
        return {xhr,sent,create:()=>xhr as unknown as XMLHttpRequest};
    }
    test('sends raw bytes with authentication and reports byte progress', async () => {
        const f=fixture(), body=new Blob([new Uint8Array([0,255,13,10])]);
        const progress:number[][]=[];
        const response=uploadWithProgress('/v1/edge/node/firmware',{method:'POST',body,headers:{Authorization:'Bearer fixture'}},(loaded,total)=>progress.push([loaded,total]),f.create);
        expect(f.sent.method).toBe('POST');expect(f.sent.body).toBe(body);expect(f.sent.headers.authorization).toBe('Bearer fixture');
        f.xhr.upload.onprogress?.({lengthComputable:true,loaded:2,total:4} as ProgressEvent);
        f.xhr.upload.onprogress?.({lengthComputable:false,loaded:3,total:0} as ProgressEvent);
        f.xhr.onload?.();
        expect(await (await response).json()).toEqual({code:0,data:{saved:true}});expect(progress).toEqual([[2,4]]);
    });
    test('cancellation stops the upload and preserves the abort reason', async () => {
        const f=fixture(),controller=new AbortController(),reason=new Error('fixture cancelled');
        const response=uploadWithProgress('/upload',{body:new Blob(['data']),signal:controller.signal},()=>{},f.create);
        controller.abort(reason);await expect(response).rejects.toBe(reason);
        const preCancelled=fixture();
        await expect(uploadWithProgress('/upload',{signal:controller.signal},()=>{},preCancelled.create)).rejects.toBe(reason);
        expect(preCancelled.sent.url).toBeUndefined();
    });
    test('returns server errors without retry and rejects lost connections', async () => {
        const f=fixture();f.xhr.status=403;f.xhr.responseText='{"code":11007,"message":"denied"}';
        const response=uploadWithProgress('/upload',{},()=>{},f.create);f.xhr.onload?.();
        expect((await response).status).toBe(403);
        const broken=fixture();const failure=uploadWithProgress('/upload',{},()=>{},broken.create);broken.xhr.onerror?.();
        await expect(failure).rejects.toThrow('上传连接失败');
    });
});
