#include "client_state_store.h"
#include "../common/files.h"
#include "../common/data_protection.h"
#include "../common/win32.h"
namespace iotvpn::service {
Json DpapiClientStateStore::load() {
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
void DpapiClientStateStore::save(const Json& state) {
    auto plaintext = state.dump();
    try {
        const auto encrypted = protectData({reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size()}, "IotEngineVpn.Agent.v1");
        SecureZeroMemory(plaintext.data(), plaintext.size()); atomicWrite(stateDirectory() / L"state.dpapi", encrypted);
    } catch (...) { SecureZeroMemory(plaintext.data(), plaintext.size()); throw; }
}

}
