#include "../service_control/snapshot.h"
#include <iostream>
using namespace iotvpn::service_control::services;
int main() {
    try {
        Snapshot s;
        s.present=true; s.running=true; s.delayed=true; s.failureNonCrash=true;
        s.type=SERVICE_WIN32_OWN_PROCESS; s.startType=SERVICE_AUTO_START; s.errorControl=SERVICE_ERROR_NORMAL;
        s.sidType=SERVICE_SID_TYPE_UNRESTRICTED; s.resetPeriod=86400;
        s.binary=L"\"C:\\Program Files\\iot-egine\\Service\\IotVpn.Service.exe\" --service";
        s.display=L"iot-egine 隧道"; s.account=L"LocalSystem";
        s.dependencies=std::wstring(L"Nsi\0TcpIp\0\0",11);
        s.actions={{SC_ACTION_RESTART,5000},{SC_ACTION_NONE,0}};
        const auto serialized=encode(s).dump();
        const auto restored=decode(iotvpn::Json::parse(serialized));
        if (encode(restored)!=encode(s) || restored.dependencies.size()!=11 || restored.actions.size()!=2)
            throw std::runtime_error("Service rollback snapshot lost configuration");
        const auto empty=decode(encode(Snapshot{}));
        if(empty.present) throw std::runtime_error("Absent service must stay absent");
        std::cout << "PASS service rollback snapshot, Unicode, MULTI_SZ, failure actions and absent service\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
