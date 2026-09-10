#include "../gui/credentials.h"
#include <iostream>
int main() {
    using iotvpn::gui::Credentials;
    Credentials store(L"IotEngineVpn/Test/"+std::to_wstring(GetCurrentProcessId()));
    struct Cleanup { Credentials& store; ~Cleanup() { try { store.clear(); } catch (...) {} } } cleanup{store};
    try {
        std::string user,password;
        store.clear();
        if (store.load(user,password)) throw std::runtime_error("Missing credential returned data");
        store.save("测试用户","test-only-秘密-123");
        if (!store.load(user,password) || user!="测试用户" || password!="test-only-秘密-123") throw std::runtime_error("Credential roundtrip failed");
        store.save("other","replacement");
        if (!store.load(user,password) || user!="other" || password!="replacement") throw std::runtime_error("Credential replacement failed");
        store.clear(); store.clear();
        if (store.load(user,password)) throw std::runtime_error("Credential removal failed");
        SecureZeroMemory(password.data(),password.size());
        std::cout<<"PASS Windows credential save, reload, replace and clear\n";
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
