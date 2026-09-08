// The three disposable local role processes must already be running.
import assert from 'node:assert/strict';
const roles=['api','media','vpn'];
const paths=['/v1/device','/v1/gb28181/devices','/v1/vpn/networks'];
for(const [index,port] of [55102,55203,55204].entries()) {
    const base=`http://127.0.0.1:${port}`;
    const health=await fetch(`${base}/internal/health/ready`,{signal:AbortSignal.timeout(5000)});
    assert.equal(health.status,200,`${roles[index]} readiness is not exposed`);
    const state=await health.json();
    console.log(roles[index],'ready',JSON.stringify(state));
    for(const [target,path] of paths.entries()) {
        const response=await fetch(base+path,{signal:AbortSignal.timeout(5000)});
        assert.equal(response.status,index===target?401:404,`${roles[index]} boundary ${path}`);
        await response.body?.cancel();
    }
}
console.log('PASS role readiness and API/media/VPN route isolation');
