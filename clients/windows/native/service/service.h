#pragma once
#include "../common/common.h"
#include <condition_variable>
#include <mutex>
#include <optional>

namespace iotvpn::service {
struct ApiError : std::runtime_error {
    int httpStatus, code;
    ApiError(std::string message, int status, int apiCode = 0)
        : std::runtime_error(std::move(message)), httpStatus(status), code(apiCode) {}
    bool isPeerUnavailable() const { return code == 21003 || code == 21004 || httpStatus == 404 || httpStatus == 410; }
    bool isAuth() const { return !isPeerUnavailable() && (httpStatus == 401 || httpStatus == 403 || code == 11002 || code == 11004 || code == 11005 || code == 11006 || code == 11007 || code == 11008); }
    bool isTokenExpired() const { return httpStatus == 401 || code == 11004 || code == 11005 || code == 11006; }
};
struct IApiTransport {
    virtual ~IApiTransport() = default;
    virtual Json login(std::string_view user, std::string_view password, std::stop_token stop) = 0;
    virtual Json refresh(std::string_view refreshToken, std::stop_token stop) = 0;
    virtual Json devices(std::string_view accessToken, std::stop_token stop) = 0;
    virtual Json apply(std::string_view accessToken, std::string_view peerId, const Json& body, std::stop_token stop) = 0;
    virtual Json config(std::string_view accessToken, std::string_view peerId, std::stop_token stop) = 0;
    virtual void remove(std::string_view accessToken, std::string_view peerId, std::stop_token stop) = 0;
    virtual void watchConfig(std::string_view accessToken, std::string_view peerId,
        const std::function<void(const Json&)>& onSnapshot, std::stop_token stop) = 0;
};
struct IStateStore {
    virtual ~IStateStore() = default;
    virtual Json load() = 0;
    virtual void save(const Json&) = 0;
};
class FileStateStore final : public IStateStore {
public: Json load() override { return loadState(); } void save(const Json& state) override { saveState(state); }
};

struct Status {
    std::string state = "LoggedOut", username, peerId, assignedIpv4, error, lastSyncAt;
    std::vector<std::string> edgeNodeIds, allowedRoutes;
    bool tunnelRunning = false;
    Json toJson() const;
};
class Coordinator {
public:
    Coordinator(std::shared_ptr<IStateStore>, std::shared_ptr<IApiTransport>, std::unique_ptr<Tunnel>);
    Json handle(const Json& request, std::stop_token stop = {});
    void run(std::stop_token stop);
    void stop();
    Status status() const;
    static bool equivalentConfig(const Json& a, const Json& b);
private:
    void invalidateWatch();
    bool shouldWatch() const;
    void requireSession() const;
    void requirePeer() const;
    Json authorized(const std::function<Json(std::string_view)>& request, std::stop_token stop);
    void refreshLocked(std::string_view expectedToken, std::stop_token stop);
    void loginLocked(const Json& request, std::stop_token stop);
    void applyLocked(const Json& request, std::stop_token stop);
    void rememberPendingRevocation(std::string_view peerId);
    void clearLocalPeer(std::string_view peerId);
    Json recoverPendingEnrollment(std::stop_token stop, bool release = false);
    void retryPendingRevocations(std::stop_token stop);
    Json disconnectLocked(std::stop_token stop);
    Json logoutLocked(std::stop_token stop);
    void acceptConfig(const Json& config, std::stop_token stop);
    void apiFailure(const ApiError& error);
    void setStatus(std::string name, std::string error = {}, bool synced = false);
    Json success() const;
    std::shared_ptr<IStateStore> store_;
    std::shared_ptr<IApiTransport> api_;
    std::unique_ptr<Tunnel> tunnel_;
    Json state_;
    mutable std::mutex gate_, statusGate_;
    std::condition_variable_any changes_;
    std::shared_ptr<std::stop_source> watch_;
    std::uint64_t generation_ = 0;
    Status status_;
};
class WinHttpTransport final : public IApiTransport {
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
bool isPlatformOrigin(std::string_view url);
std::string canonicalId(std::string_view id);
int runService(int argc, wchar_t** argv);
}
