#pragma once
#include "../common/common.h"
#include "../common/win32.h"
#include <wincred.h>

namespace iotvpn::gui {
// Generic credentials are encrypted by Windows and scoped to the current user.
class Credentials {
public:
    explicit Credentials(std::wstring target = L"IotEngineVpn/DesktopLogin") : target_(std::move(target)) {}
    bool load(std::string& username, std::string& password) const {
        PCREDENTIALW entry{};
        if (!CredReadW(target_.c_str(), CRED_TYPE_GENERIC, 0, &entry)) {
            if (GetLastError() == ERROR_NOT_FOUND) return false;
            win32Error("无法读取已保存的账号");
        }
        struct Guard { PCREDENTIALW value; ~Guard() { if (value->CredentialBlob) SecureZeroMemory(value->CredentialBlob,value->CredentialBlobSize); CredFree(value); } } guard{entry};
        username = entry->UserName ? utf8(entry->UserName) : std::string{};
        password.assign(reinterpret_cast<const char*>(entry->CredentialBlob),entry->CredentialBlobSize);
        return true;
    }
    void save(const std::string& username, const std::string& password) const {
        if (password.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) throw std::runtime_error("密码过长，无法保存。");
        auto user = utf16(username);
        CREDENTIALW entry{}; entry.Type = CRED_TYPE_GENERIC;
        entry.TargetName = const_cast<wchar_t*>(target_.c_str()); entry.UserName = user.data();
        entry.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(password.data()));
        entry.CredentialBlobSize = static_cast<DWORD>(password.size()); entry.Persist = CRED_PERSIST_LOCAL_MACHINE;
        if (!CredWriteW(&entry,0)) win32Error("无法保存账号和密码");
    }
    void clear() const {
        if (!CredDeleteW(target_.c_str(),CRED_TYPE_GENERIC,0) && GetLastError()!=ERROR_NOT_FOUND) win32Error("无法清除已保存的账号");
    }
private:
    std::wstring target_;
};
}
