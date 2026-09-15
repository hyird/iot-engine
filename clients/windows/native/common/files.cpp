#include "files.h"
#include "win32.h"
#include <sddl.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

namespace iotvpn {
std::filesystem::path productDataDirectory() {
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


}
