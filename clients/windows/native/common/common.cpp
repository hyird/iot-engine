#include "common.h"
#include "win32.h"
#include <sddl.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

namespace iotvpn {
std::wstring utf16(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!size) win32Error("Invalid UTF-8");
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}
std::string utf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) win32Error("Invalid Unicode");
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}
std::filesystem::path stateDirectory() {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &value))) throw std::runtime_error("Cannot locate Windows state directory");
    std::filesystem::path result(value); CoTaskMemFree(value);
    return result / L"IotEngineVpn";
}
std::filesystem::path moduleDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) win32Error("Cannot locate application");
    path.resize(count); return std::filesystem::path(path).parent_path();
}
Json parseJson(std::string_view text) {
    if (text.size() > MaxMessageBytes) throw std::runtime_error("Message exceeds 1 MiB");
    return Json::parse(text, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 16) throw std::runtime_error("JSON nesting exceeds limit");
        return true;
    });
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path, std::size_t limit) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!file) win32Error("Cannot read file");
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("File cannot be a reparse point");
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.value, &length) || length.QuadPart < 0 || static_cast<std::uint64_t>(length.QuadPart) > limit)
        throw std::runtime_error("File exceeds size limit");
    std::vector<std::uint8_t> data(static_cast<std::size_t>(length.QuadPart));
    DWORD read = 0;
    if (!data.empty() && (!ReadFile(file.value, data.data(), static_cast<DWORD>(data.size()), &read, nullptr) || read != data.size()))
        win32Error("Cannot read complete file");
    return data;
}
void atomicWrite(const std::filesystem::path& path, std::span<const std::uint8_t> data) {
    const auto temporary = std::filesystem::path(path.wstring() + L".new");
    {
        Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!file) win32Error("Cannot stage file");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file.value, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("State file cannot be a reparse point");
        DWORD written = 0;
        if (!WriteFile(file.value, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) || written != data.size() || !FlushFileBuffers(file.value))
            win32Error("Cannot save state");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) win32Error("Cannot replace state");
}

std::vector<std::uint8_t> protectData(std::span<const std::uint8_t> data, std::string_view entropy, std::wstring_view description) {
    DATA_BLOB input{ static_cast<DWORD>(data.size()), const_cast<BYTE*>(data.data()) };
    DATA_BLOB extra{ static_cast<DWORD>(entropy.size()), reinterpret_cast<BYTE*>(const_cast<char*>(entropy.data())) }, output{};
    const std::wstring caption(description);
    if (!CryptProtectData(&input, caption.c_str(), entropy.empty() ? nullptr : &extra, nullptr, nullptr,
        CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN, &output)) win32Error("DPAPI encryption failed");
    std::vector<std::uint8_t> result(output.pbData, output.pbData + output.cbData); LocalFree(output.pbData); return result;
}
std::vector<std::uint8_t> unprotectData(std::span<const std::uint8_t> data, std::string_view entropy) {
    DATA_BLOB input{ static_cast<DWORD>(data.size()), const_cast<BYTE*>(data.data()) };
    DATA_BLOB extra{ static_cast<DWORD>(entropy.size()), reinterpret_cast<BYTE*>(const_cast<char*>(entropy.data())) }, output{};
    if (!CryptUnprotectData(&input, nullptr, entropy.empty() ? nullptr : &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output))
        win32Error("DPAPI decryption failed");
    std::vector<std::uint8_t> result(output.pbData, output.pbData + output.cbData);
    SecureZeroMemory(output.pbData, output.cbData); LocalFree(output.pbData); return result;
}
Json loadState() {
    const auto path = stateDirectory() / L"state.dpapi";
    if (!std::filesystem::exists(path)) return Json::object();
    auto plaintext = unprotectData(readFile(path), "IotEngineVpn.Agent.v1");
    try {
        auto state = parseJson({reinterpret_cast<const char*>(plaintext.data()), plaintext.size()});
        SecureZeroMemory(plaintext.data(), plaintext.size());
        if (!state.is_object()) throw std::runtime_error("Invalid stored client state");
        return state;
    } catch (...) { SecureZeroMemory(plaintext.data(), plaintext.size()); throw; }
}
void saveState(const Json& state) {
    auto plaintext = state.dump();
    try {
        const auto encrypted = protectData({reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size()}, "IotEngineVpn.Agent.v1");
        SecureZeroMemory(plaintext.data(), plaintext.size()); atomicWrite(stateDirectory() / L"state.dpapi", encrypted);
    } catch (...) { SecureZeroMemory(plaintext.data(), plaintext.size()); throw; }
}

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
    if (!service) throw std::runtime_error("请先安装 iot-egine 网络服务。");
    SERVICE_STATUS_PROCESS status{}; DWORD needed = 0;
    if (!QueryServiceStatusEx(service.value, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed))
        win32Error("Cannot identify local service");
    if (!status.dwProcessId || status.dwProcessId != serverPid) throw std::runtime_error("本机管道身份验证失败。");
}
}

Json pipeRequest(const Json& request, std::uint32_t timeoutMs, std::stop_token stop) {
    if (stop.stop_requested()) throw std::runtime_error("Operation cancelled");
    if (!WaitNamedPipeW(PipeName, 5000) && GetLastError() != ERROR_SEM_TIMEOUT)
        throw std::runtime_error("网络服务尚未启动，请重新运行安装程序。");
    Handle pipe(CreateFileW(PipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    if (!pipe) throw std::runtime_error("无法连接网络服务，请稍后重试。");
    verifyServer(pipe.value);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    writeFrame(pipe.value, request, deadline, stop);
    auto response = readFrame(pipe.value, deadline, stop);
    // Tell the server the buffered response has been consumed before it disconnects.
    // Older controllers may close immediately; a failed acknowledgment does not invalidate a complete response.
    try { char acknowledged = 0; transfer(pipe.value, &acknowledged, 1, true, deadline, stop); } catch (...) { }
    return response;
}

void runPipeServer(std::stop_token stop, const std::function<Json(const Json&)>& handler) {
    const auto ownerData = readFile(stateDirectory() / L"owner.sid", 512);
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
