#include "files.h"
#include "text.h"
#include "ipc.h"
#include "product.h"
#include "win32.h"
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <thread>

namespace iotvpn {
namespace {
using Deadline = std::chrono::steady_clock::time_point;
DWORD waitIo(HANDLE file, OVERLAPPED& operation, Deadline deadline, std::stop_token stop) {
    while (WaitForSingleObject(operation.hEvent, 100) == WAIT_TIMEOUT) {
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
            CancelIoEx(file, &operation);
            DWORD ignored = 0; GetOverlappedResult(file, &operation, &ignored, TRUE);
            throw std::runtime_error(stop.stop_requested() ? "Operation cancelled" : "Local service request timed out");
        }
    }
    DWORD count = 0;
    if (!GetOverlappedResult(file, &operation, &count, FALSE)) win32Error("Local service I/O failed");
    return count;
}
DWORD transfer(HANDLE pipe, void* bytes, DWORD size, bool write, Deadline deadline, std::stop_token stop) {
    Handle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!ready) win32Error("Cannot create local I/O event");
    OVERLAPPED operation{}; operation.hEvent = ready.value;
    DWORD count = 0;
    const BOOL success = write ? WriteFile(pipe, bytes, size, &count, &operation) : ReadFile(pipe, bytes, size, &count, &operation);
    if (!success && GetLastError() != ERROR_IO_PENDING) win32Error("Local service communication failed");
    return success ? count : waitIo(pipe, operation, deadline, stop);
}
Json readFrame(HANDLE pipe, Deadline deadline, std::stop_token stop = {}) {
    std::string message;
    for (;;) {
        char buffer[8192];
        const DWORD count = transfer(pipe, buffer, sizeof(buffer), false, deadline, stop);
        if (!count) throw std::runtime_error("Local service closed its response");
        const auto newline = std::find(buffer, buffer + count, '\n');
        message.append(buffer, newline);
        if (message.size() > MaxMessageBytes) throw std::runtime_error("Local service frame exceeds 1 MiB");
        if (newline != buffer + count) return parseJson(message);
    }
}
void writeFrame(HANDLE pipe, const Json& value, Deadline deadline, std::stop_token stop = {}) {
    auto message = value.dump();
    if (message.size() > MaxMessageBytes) throw std::runtime_error("Local service frame exceeds 1 MiB");
    message += '\n';
    std::size_t offset = 0;
    while (offset < message.size()) {
        const auto count = transfer(pipe, message.data() + offset, static_cast<DWORD>(message.size() - offset), true, deadline, stop);
        if (!count) throw std::runtime_error("Local service closed its request");
        offset += count;
    }
    SecureZeroMemory(message.data(), message.size());
}
void verifyServer(HANDLE pipe) {
    ULONG serverPid = 0;
    if (!GetNamedPipeServerProcessId(pipe, &serverPid)) win32Error("Cannot verify local service");
    ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) win32Error("Cannot access Windows services");
    ServiceHandle service(OpenServiceW(manager.value, AgentService, SERVICE_QUERY_STATUS));
    if (!service) throw LocalServiceError("本机网络服务未安装，请点击修复服务。");
    SERVICE_STATUS_PROCESS status{}; DWORD needed = 0;
    if (!QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed))
        win32Error("Cannot identify local service");
    if (!status.dwProcessId || status.dwProcessId != serverPid) throw LocalServiceError("本机管道身份验证失败，请点击修复服务。");
}
}

Json pipeRequest(const Json& request, std::uint32_t timeoutMs, std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Operation cancelled");
    if (!WaitNamedPipeW(PipeName, 5000) && GetLastError() != ERROR_SEM_TIMEOUT)
        throw LocalServiceError("网络服务尚未启动，请点击修复服务。");
    Handle pipe(CreateFileW(PipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    if (!pipe) throw LocalServiceError("无法连接网络服务，请点击修复服务。");
    try { verifyServer(pipe.value); }
    catch (const LocalServiceError&) { throw; }
    catch (const std::exception&) { throw LocalServiceError("无法验证本机网络服务，请点击修复服务。"); }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    writeFrame(pipe.value, request, deadline, stop);
    auto response = readFrame(pipe.value, deadline, stop);
    // Tell the server the buffered response has been consumed before it disconnects.
    // Older controllers may close immediately; a failed acknowledgment does not invalidate a complete response.
    try { char acknowledged = 0; transfer(pipe.value, &acknowledged, 1, true, deadline, stop); } catch (...) { }
    return response;
}

ServiceRepairResult repairLocalService() {
    ServiceRepairResult result;
    const auto root = productInstallRoot();
    const auto exe = root / L"Service" / L"IotVpn.ServiceControl.exe";
    if (!std::filesystem::is_regular_file(exe)) {
        result.message = "未找到服务修复程序，请重新安装客户端。";
        return result;
    }
    wchar_t tempDirectory[MAX_PATH]{};
    const auto tempLength = GetTempPathW(MAX_PATH, tempDirectory);
    if (!tempLength || tempLength >= MAX_PATH) {
        result.message = "无法创建服务修复报告。";
        return result;
    }
    const auto report = (std::filesystem::path(tempDirectory) / (L"iot-egine-repair-" + std::to_wstring(GetCurrentProcessId()) + L".txt")).wstring();
    DeleteFileW(report.c_str());
    const auto parameters = L"--repair --report \"" + report + L"\"";
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = parameters.c_str();
    info.lpDirectory = root.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        if (GetLastError() == ERROR_CANCELLED) {
            result.cancelled = true;
            result.message = "已取消服务修复。";
            return result;
        }
        result.message = "无法启动服务修复程序。";
        return result;
    }
    Handle process(info.hProcess);
    if (WaitForSingleObject(process.value, 120000) == WAIT_TIMEOUT) {
        result.message = "服务修复超时，请稍后重试。";
        return result;
    }
    DWORD exitCode = 1;
    GetExitCodeProcess(process.value, &exitCode);
    std::string reportText;
    if (std::filesystem::is_regular_file(report)) {
        try {
            const auto bytes = readFile(report, 4096);
            reportText.assign(bytes.begin(), bytes.end());
        } catch (...) {}
        DeleteFileW(report.c_str());
    }
    while (!reportText.empty() && (reportText.back() == '\n' || reportText.back() == '\r')) reportText.pop_back();
    if (exitCode == 0) {
        result.succeeded = true;
        result.message = "本机服务已修复。";
        return result;
    }
    if (reportText.starts_with("FAIL ")) result.message = reportText.substr(5);
    else if (!reportText.empty()) result.message = reportText;
    else result.message = "服务修复失败，请稍后重试。";
    return result;
}

void runPipeServer(std::stop_token stop, const std::function<Json(const Json&)>& handler) {
    const auto ownerData = readFile(productDataDirectory() / L"owner.sid", 512);
    std::string owner(ownerData.begin(), ownerData.end());
    while (!owner.empty() && (owner.back() == '\r' || owner.back() == '\n' || owner.back() == ' ')) owner.pop_back();
    PSID ownerSid = nullptr;
    const auto wideOwner = utf16(owner);
    if (!ConvertStringSidToSidW(wideOwner.c_str(), &ownerSid)) throw std::runtime_error("Invalid installed Windows owner");
    LocalFree(ownerSid);
    const auto sddl = L"D:P(D;;GA;;;NU)(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;" + wideOwner + L")";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) win32Error("Cannot secure local service");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    Handle pipe(CreateNamedPipeW(PipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &security));
    LocalFree(descriptor);
    if (!pipe) win32Error("Cannot create protected local service pipe");
    while (!stop.stop_requested()) {
        Handle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!ready) win32Error("Cannot create pipe event");
        OVERLAPPED operation{}; operation.hEvent = ready.value;
        const BOOL connected = ConnectNamedPipe(pipe.value, &operation);
        const auto error = connected ? ERROR_SUCCESS : GetLastError();
        try {
            if (!connected && error == ERROR_IO_PENDING) waitIo(pipe.value, operation, Deadline::max(), stop);
            else if (!connected && error != ERROR_PIPE_CONNECTED) win32Error("Cannot accept local client", error);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(85);
            Json response;
            try { response = handler(readFrame(pipe.value, deadline, stop)); }
            catch (const std::exception& exception) { response = { {"success", false}, {"message", exception.what()} }; }
            writeFrame(pipe.value, response, deadline, stop);
            // Also supports older clients: they close after reading, causing EOF here.
            // Never use unbounded FlushFileBuffers on a pipe owned by a GUI client.
            char acknowledged = 0;
            transfer(pipe.value, &acknowledged, 1, false, deadline, stop);
        } catch (...) { if (stop.stop_requested()) break; }
        DisconnectNamedPipe(pipe.value);
    }
}

}
