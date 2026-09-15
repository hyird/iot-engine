#pragma once
#include "../common/json.h"
#include <functional>
#include <stop_token>
namespace iotvpn::service {
struct PlatformVpnApiError : std::runtime_error {
    int httpStatus, code;
    PlatformVpnApiError(std::string message, int status, int apiCode = 0)
        : std::runtime_error(std::move(message)), httpStatus(status), code(apiCode) {}
    bool isPeerUnavailable() const { return code == 21003 || code == 21004 || httpStatus == 404 || httpStatus == 410; }
    bool isAuth() const { return !isPeerUnavailable() && (httpStatus == 401 || httpStatus == 403 || code == 11002 || code == 11004 || code == 11005 || code == 11006 || code == 11007 || code == 11008); }
    bool isTokenExpired() const { return httpStatus == 401 || code == 11004 || code == 11005 || code == 11006; }
};
struct IPlatformVpnApi {
    virtual ~IPlatformVpnApi() = default;
    virtual Json login(std::string_view user, std::string_view password, std::stop_token stop) = 0;
    virtual Json refresh(std::string_view refreshToken, std::stop_token stop) = 0;
    virtual Json devices(std::string_view accessToken, std::stop_token stop) = 0;
    virtual Json apply(std::string_view accessToken, std::string_view peerId, const Json& body, std::stop_token stop) = 0;
    virtual Json config(std::string_view accessToken, std::string_view peerId, std::stop_token stop) = 0;
    virtual void remove(std::string_view accessToken, std::string_view peerId, std::stop_token stop) = 0;
    virtual void watchConfig(std::string_view accessToken, std::string_view peerId,
        const std::function<void(const Json&)>& onSnapshot, std::stop_token stop) = 0;
};
class WinHttpPlatformVpnApi final : public IPlatformVpnApi {
public:
    Json login(std::string_view, std::string_view, std::stop_token) override;
    Json refresh(std::string_view, std::stop_token) override;
    Json devices(std::string_view, std::stop_token) override;
    Json apply(std::string_view, std::string_view, const Json&, std::stop_token) override;
    Json config(std::string_view, std::string_view, std::stop_token) override;
    void remove(std::string_view, std::string_view, std::stop_token) override;
    void watchConfig(std::string_view, std::string_view, const std::function<void(const Json&)>&, std::stop_token) override;
};
class SseParser {
public:
    // Returning false stops after the first snapshot, used for one-shot SSE reads.
    bool feed(std::string_view bytes, const std::function<bool(std::string_view, std::string_view)>& callback);
private: std::string line_, data_, event_ = "message";
};
std::string canonicalId(std::string_view id);
}
