#include "native_services.h"
#include "snapshot.h"
#include "safe_files.h"
#include "../common/common.h"
#include "../common/win32.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <wintrust.h>
#include <softpub.h>
#include <sddl.h>
#include <tlhelp32.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace sc = iotvpn::service_control;
namespace {
constexpr wchar_t Agent[] = L"iot-egine.tunnel";

struct Options { std::wstring command, owner, root, report; };
struct SessionOwner { std::wstring sid; DWORD session{}; };

void fail(const char* message) { throw std::runtime_error(message); }
bool equalInsensitive(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
fs::path installRoot() {
    PWSTR value{}; if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &value))) fail("Cannot locate Program Files");
    fs::path result(value); CoTaskMemFree(value); return result / L"iot-egine";
}
fs::path stateRoot() {
    PWSTR value{}; if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &value))) fail("Cannot locate ProgramData");
    fs::path result(value); CoTaskMemFree(value); return result / L"IotEngineVpn";
}
fs::path serviceExe() { return installRoot()/L"Service/IotVpn.Service.exe"; }
std::wstring quoted(fs::path path) { return L"\"" + path.make_preferred().wstring() + L"\""; }
std::wstring agentCommand() { return quoted(serviceExe()) + L" --service"; }

void requireAdmin() {
    BOOL member = FALSE; SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY; PSID administrators{};
    if (!AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0,0,0,0,0,0, &administrators))
        fail("Cannot inspect administrator membership");
    const bool result = CheckTokenMembership(nullptr, administrators, &member) && member;
    FreeSid(administrators); if (!result) fail("Administrator privileges are required");
}
std::wstring sidForAccount(const std::wstring& domain, const std::wstring& user) {
    const auto account = domain.empty() ? user : domain + L"\\" + user;
    DWORD size = 0, domainSize = 0; SID_NAME_USE type{};
    LookupAccountNameW(nullptr, account.c_str(), nullptr, &size, nullptr, &domainSize, &type);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) fail("Cannot resolve interactive Windows account");
    std::vector<BYTE> bytes(size); std::vector<wchar_t> domainName(domainSize);
    if (!LookupAccountNameW(nullptr, account.c_str(), bytes.data(), &size, domainName.data(), &domainSize, &type) || type != SidTypeUser)
        fail("Interactive account is not a user");
    LPWSTR text{}; if (!ConvertSidToStringSidW(bytes.data(), &text)) fail("Cannot encode owner SID");
    std::wstring result(text); LocalFree(text); return result;
}
std::wstring canonicalSid(const std::wstring& value);
SessionOwner interactiveOwner() {
    DWORD session{}; if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) fail("Cannot identify Windows session");
    if (!session) fail("Non-interactive installation requires an explicit --owner SID");
    struct WtsBuffer { LPWSTR value{}; ~WtsBuffer() { if (value) WTSFreeMemory(value); } } user, domain;
    DWORD userSize = 0, domainSize = 0;
    const bool queried = WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSUserName, &user.value, &userSize) &&
        WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSDomainName, &domain.value, &domainSize) && user.value && user.value[0];
    if (queried) {
        try { return {sidForAccount(domain.value ? domain.value : L"", user.value), session}; } catch (const std::exception&) {}
    }
    const auto shell = GetShellWindow(); DWORD shellPid{};
    if (!shell || !GetWindowThreadProcessId(shell, &shellPid) || !shellPid) fail("Cannot identify interactive Windows user; pass --owner SID");
    DWORD shellSession{}; if (!ProcessIdToSessionId(shellPid, &shellSession)) fail("Cannot identify interactive Windows session");
    if (shellSession != session) fail("Interactive shell belongs to a different Windows session");
    iotvpn::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shellPid)); if (!process) fail("Cannot identify interactive Windows user; pass --owner SID");
    HANDLE token{}; if (!OpenProcessToken(process.value, TOKEN_QUERY, &token)) fail("Cannot identify interactive Windows user; pass --owner SID");
    iotvpn::Handle tokenHandle(token); DWORD size = 0; GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> bytes(size); if (!GetTokenInformation(token, TokenUser, bytes.data(), size, &size)) fail("Cannot identify interactive Windows user; pass --owner SID");
    LPWSTR text{}; if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &text)) fail("Cannot encode owner SID");
    std::wstring result(text); LocalFree(text); return {canonicalSid(result), shellSession};
}
std::wstring canonicalSid(const std::wstring& value) {
    PSID sid{}; if (!ConvertStringSidToSidW(value.c_str(), &sid) || !IsValidSid(sid)) { if (sid) LocalFree(sid); fail("Owner SID is invalid"); }
    DWORD nameSize = 0, domainSize = 0; SID_NAME_USE type{};
    LookupAccountSidW(nullptr, sid, nullptr, &nameSize, nullptr, &domainSize, &type);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) { LocalFree(sid); fail("Owner SID is invalid"); }
    std::vector<wchar_t> name(nameSize), domain(domainSize);
    if (!LookupAccountSidW(nullptr, sid, name.data(), &nameSize, domain.data(), &domainSize, &type) || type != SidTypeUser) { LocalFree(sid); fail("Owner SID is not a user"); }
    LPWSTR text{}; if (!ConvertSidToStringSidW(sid, &text)) { LocalFree(sid); fail("Owner SID is invalid"); }
    std::wstring result(text); LocalFree(text); LocalFree(sid); return result;
}
void validateOwner(const std::wstring& owner) {
    const auto canonical = canonicalSid(owner);
    if (!canonical.starts_with(L"S-1-5-21-") && !canonical.starts_with(L"S-1-12-1-")) fail("Owner must be a normal Windows user SID");
}
void validateStateOwner(const std::wstring& owner) {
    const auto file = stateRoot()/L"owner.sid";
    if (!fs::exists(file)) return;
    const auto bytes = iotvpn::readFile(file, 512); auto saved = iotvpn::utf16(std::string(bytes.begin(), bytes.end()));
    while (!saved.empty() && iswspace(saved.back())) saved.pop_back();
    if (!equalInsensitive(canonicalSid(saved), canonicalSid(owner))) fail("Installed owner SID does not match");
}
void validateRoots() {
    // Existing ACLs are repaired by install(); they are not an installation prerequisite.
    sc::safe_files::checkTree(installRoot()); sc::safe_files::checkTree(stateRoot());
}
void validateServices() {
    sc::services::validateExisting(Agent, agentCommand());
}
void ensureGuiClosed() {
    iotvpn::Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)); if (!snapshot) fail("Cannot inspect running applications");
    PROCESSENTRY32W entry{sizeof(entry)}; if (!Process32FirstW(snapshot.value, &entry)) fail("Cannot inspect running applications");
    do {
        iotvpn::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID)); if (!process) continue;
        wchar_t name[32768]{}; DWORD size = 32768;
        if (QueryFullProcessImageNameW(process.value, 0, name, &size) && _wcsicmp(fs::path(name).parent_path().c_str(), (installRoot()/L"Gui").c_str()) == 0)
            fail("Close iot-egine before changing services");
    } while (Process32NextW(snapshot.value, &entry));
}
void verifySignature(const fs::path& file) {
    WINTRUST_FILE_INFO info{sizeof(info), file.c_str(), nullptr, nullptr}; WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust); trust.dwUIChoice = WTD_UI_NONE; trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE; trust.pFile = &info; trust.dwStateAction = WTD_STATEACTION_VERIFY; trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2; const auto status = WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust);
    if (status != ERROR_SUCCESS) fail("WireGuard driver signature is invalid");
}
void verifyFiles(const fs::path& root) {
    sc::safe_files::checkTree(root);
    const std::array<fs::path, 5> files{root/L"Gui/iot-egine.exe", root/L"Service/IotVpn.Service.exe",
        root/L"Service/tunnel.dll", root/L"Service/wireguard.dll", root/L"Service/IotVpn.ServiceControl.exe"};
    for (const auto& file : files) { if (!fs::is_regular_file(file)) fail("Package is missing a required file"); }
    verifySignature(files[3]);}
void writeReport(const fs::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc); if (!output) fail("Cannot write report");
    output << text << "\n"; if (!output) fail("Cannot write report");
}
Options parse(int argc, wchar_t** argv) {
    Options result; std::array<bool, 4> seen{};
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg(argv[i]);
        if (arg == L"--prepare" || arg == L"--install" || arg == L"--remove" || arg == L"--stop" || arg == L"--verify-files" || arg == L"--probe" || arg == L"--rollback" || arg == L"--commit") {
            if (!result.command.empty()) fail("Exactly one command is required"); result.command = arg; continue;
        }
        if (arg == L"--owner" || arg == L"--root" || arg == L"--report") {
            const std::size_t index = arg == L"--owner" ? 0 : arg == L"--root" ? 1 : 2;
            if (seen[index] || ++i >= argc || argv[i][0] == L'-') fail("Invalid or duplicate option");
            seen[index] = true; (index == 0 ? result.owner : index == 1 ? result.root : result.report) = argv[i]; continue;
        }
        fail("Unknown argument");
    }
    if (result.command.empty()) fail("A command is required");
    if (result.command == L"--verify-files" && result.root.empty()) fail("--verify-files requires --root");
    if (result.command != L"--verify-files" && !result.root.empty()) fail("--root is only valid with --verify-files");
    if (result.command != L"--prepare" && result.command != L"--install" && !result.owner.empty()) fail("--owner is only valid with --prepare or --install");
    return result;
}
const std::array<std::wstring, 1> ServiceNames{Agent};
void saveSnapshot() {
    sc::safe_files::createProtectedDirectory(stateRoot(), false);
    iotvpn::Json j;
    for (const auto& name : ServiceNames) j[iotvpn::utf8(name)] = sc::services::encodeServiceSnapshot(sc::services::capture(name));
    const auto data = j.dump();
    iotvpn::atomicWrite(stateRoot()/L"installer-rollback.json", {reinterpret_cast<const std::uint8_t*>(data.data()), data.size()});
}
void stopAll() { for (const auto& name : ServiceNames) sc::services::ensureStopped(name); }
void rollback() {
    requireAdmin(); validateRoots();
    const auto path = stateRoot()/L"installer-rollback.json";
    if (!fs::exists(path)) return;
    const auto bytes = iotvpn::readFile(path);
    const auto j = iotvpn::Json::parse(bytes);
    stopAll();
    for (const auto& name : ServiceNames) sc::services::restore(name, sc::services::decodeServiceSnapshot(j.at(iotvpn::utf8(name))));
    for (const auto& name : ServiceNames) if (sc::services::decodeServiceSnapshot(j.at(iotvpn::utf8(name))).running) sc::services::start(name);
    fs::remove(path);
}
void commit() { requireAdmin(); validateRoots(); fs::remove(stateRoot()/L"installer-rollback.json"); }
void prepare(const std::wstring& owner) {
    requireAdmin(); validateOwner(owner); validateRoots(); validateStateOwner(owner); validateServices(); ensureGuiClosed();
    saveSnapshot(); stopAll();
}
void stop() {
    requireAdmin(); validateRoots(); validateServices();
    stopAll();
}
void install(const std::wstring& owner) {
    requireAdmin(); validateOwner(owner); validateRoots(); validateStateOwner(owner); ensureGuiClosed();
    if (!fs::is_regular_file(serviceExe())) fail("Installed service files are missing");
    verifySignature(installRoot()/L"Service/wireguard.dll");
    validateServices();
    const auto oldAgent = sc::services::capture(Agent);
    try {
        sc::safe_files::createProtectedDirectory(installRoot(), true);
        sc::safe_files::createProtectedDirectory(stateRoot(), false);
        if (!fs::exists(stateRoot()/L"owner.sid")) { const auto text = iotvpn::utf8(owner); iotvpn::atomicWrite(stateRoot()/L"owner.sid", {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}); }
        sc::services::installOrUpdate(Agent, L"iot-egine", agentCommand(), true, {L"Nsi", L"TcpIp"});
        sc::services::start(Agent);
    } catch (...) {
        try { sc::services::ensureStopped(Agent); sc::services::restore(Agent, oldAgent); } catch (...) {}
        throw;
    }
}
void removeServices() {
    requireAdmin(); validateRoots(); validateServices(); ensureGuiClosed(); stopAll(); for (const auto& name : ServiceNames) sc::services::remove(name);
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    std::wstring report;
    bool parsing = true;
    try {
        int argc{}; auto argv = CommandLineToArgvW(GetCommandLineW(), &argc); if (!argv) return 2;
        Options options;
        try { options = parse(argc, argv); } catch (...) { LocalFree(argv); throw; }
        report = options.report; LocalFree(argv); parsing = false;
        if (options.command == L"--probe") {
            const auto owner = interactiveOwner(); if (!options.report.empty()) writeReport(options.report, "PASS sid=" + iotvpn::utf8(owner.sid) + " session=" + std::to_string(owner.session));
        } else if (options.command == L"--verify-files") {
            verifyFiles(fs::absolute(options.root)); if (!options.report.empty()) writeReport(options.report, "PASS files");
        } else if (options.command == L"--prepare") {
            prepare(options.owner.empty() ? interactiveOwner().sid : canonicalSid(options.owner)); if (!options.report.empty()) writeReport(options.report, "PASS prepared");
        } else if (options.command == L"--install") {
            install(options.owner.empty() ? interactiveOwner().sid : canonicalSid(options.owner)); if (!options.report.empty()) writeReport(options.report, "PASS installed");
        } else if (options.command == L"--rollback") {
            rollback();
        } else if (options.command == L"--commit") {
            commit();
        } else if (options.command == L"--stop") {
            stop(); if (!options.report.empty()) writeReport(options.report, "PASS stopped");
        } else {
            removeServices(); if (!options.report.empty()) writeReport(options.report, "PASS removed");
        }
        return 0;
    } catch (const std::exception& error) {
        if (!report.empty()) {
            try { writeReport(report, std::string("FAIL ") + error.what()); } catch (...) {}
        }
        return parsing ? 2 : 1;
    }
}
