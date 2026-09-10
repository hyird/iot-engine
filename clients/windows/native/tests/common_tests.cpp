#include "../common/common.h"
#include <iostream>
#include <set>
using namespace iotvpn;
static void ok(bool x,const char*m){if(!x)throw std::runtime_error(m);}
int main(){int bad=0;try{std::vector<unsigned char> p={'s','e','c','r','e','t'};auto c=protectData(p);ok(c!=p,"DPAPI no-op");ok(unprotectData(c)==p,"DPAPI roundtrip");std::cout<<"PASS DPAPI\n";}catch(const std::exception&e){++bad;std::cout<<"FAIL DPAPI: "<<e.what()<<"\n";}
try{auto t=createTunnel();std::set<std::string> keys;for(int i=0;i<16;++i){auto k=t->generateKeys();ok(k.first.size()==44&&k.second.size()==44,"key size");keys.insert(k.first);keys.insert(k.second);}ok(keys.size()==32,"key uniqueness");std::cout<<"PASS native keys\n";}catch(const std::exception&e){++bad;std::cout<<"FAIL native keys: "<<e.what()<<"\n";}
try{Json c={{"peerId","11111111-1111-4111-8111-111111111111"},{"publicKey","AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE="},{"hubPublicKey","AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE="},{"assignedIpv4","100.96.0.2"},{"hubEndpoint","vpn.example"},{"hubListenPort",51820},{"mtu",1420},{"persistentKeepalive",25},{"edgeNodeIds",{"11111111-1111-4111-8111-111111111111"}},{"edgeAddresses",{"100.96.0.3"}},{"allowedRoutes",{"100.96.0.3/32"}}};validateTunnelConfig(c,"AQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQEBAQE=");ok(renderTunnelConfig(c,c["publicKey"].get<std::string>()).find("Endpoint")!=std::string::npos,"render");std::cout<<"PASS route validation\n";}catch(const std::exception&e){++bad;std::cout<<"FAIL route validation: "<<e.what()<<"\n";}return bad?1:0;}
