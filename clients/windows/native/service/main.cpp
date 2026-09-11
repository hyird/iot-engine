#include "service.h"
#include "../common/win32.h"
#include <atomic>
#include <thread>

namespace iotvpn::service {
namespace {
std::stop_source stopSource;
SERVICE_STATUS_HANDLE serviceHandle = nullptr;
std::mutex reportGate;
std::atomic<DWORD> serviceError{0};
void report(DWORD state, DWORD error = NO_ERROR) {
    std::lock_guard lock(reportGate);
    SERVICE_STATUS status{}; status.dwServiceType = SERVICE_WIN32_OWN_PROCESS; status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    status.dwWin32ExitCode = error;
    if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) { status.dwWaitHint = 30000; status.dwCheckPoint = 1; }
    if (serviceHandle) SetServiceStatus(serviceHandle, &status);
}
DWORD WINAPI control(DWORD code, DWORD, LPVOID, LPVOID) {
    if (code == SERVICE_CONTROL_STOP || code == SERVICE_CONTROL_SHUTDOWN) { report(SERVICE_STOP_PENDING); stopSource.request_stop(); return NO_ERROR; }
    if (code == SERVICE_CONTROL_INTERROGATE) return NO_ERROR;
    return ERROR_CALL_NOT_IMPLEMENTED;
}
void WINAPI serviceMain(DWORD, LPWSTR*) {
    serviceHandle = RegisterServiceCtrlHandlerExW(AgentService, control, nullptr);
    if (!serviceHandle) { serviceError = GetLastError(); return; }
    report(SERVICE_START_PENDING);
    try {
        SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        VpnConnectionService vpnService(std::make_shared<DpapiClientStateStore>(), std::make_shared<WinHttpPlatformVpnApi>(), createWindowsWireGuardTunnel());
        std::jthread sync([&] {
            try { vpnService.run(stopSource.get_token()); }
            catch (...) { serviceError = ERROR_SERVICE_SPECIFIC_ERROR; stopSource.request_stop(); }
        });
        report(SERVICE_RUNNING);
        try { runPipeServer(stopSource.get_token(), [&](const Json& request) { return vpnService.handle(request, stopSource.get_token()); }); }
        catch (...) { serviceError = ERROR_SERVICE_SPECIFIC_ERROR; }
        stopSource.request_stop();
        try { vpnService.stop(); } catch (...) { serviceError = ERROR_SERVICE_SPECIFIC_ERROR; }
        sync.join();
    } catch (...) { serviceError = ERROR_SERVICE_SPECIFIC_ERROR; stopSource.request_stop(); }
    report(SERVICE_STOPPED, serviceError.load());
}
}
int runWindowsVpnService(int argc, wchar_t** argv) {
    if (argc != 2 || std::wstring_view(argv[1]) != L"--service") return 2;
    SERVICE_TABLE_ENTRYW entries[]{{const_cast<LPWSTR>(AgentService), serviceMain}, {nullptr, nullptr}};
    if (!StartServiceCtrlDispatcherW(entries)) return static_cast<int>(GetLastError());
    return static_cast<int>(serviceError.load());
}
}

int wmain(int argc, wchar_t** argv) {
    return iotvpn::service::runWindowsVpnService(argc, argv);
}
