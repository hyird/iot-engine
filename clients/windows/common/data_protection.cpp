#include "data_protection.h"
#include "win32.h"
#include <sddl.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

namespace iotvpn {
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

}
