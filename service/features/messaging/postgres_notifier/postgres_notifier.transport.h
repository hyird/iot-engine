#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <thread>

#include <ruvia/web/db/DbTypes.h>

namespace service::message::outbox {

class PostgresNotifier final {
public:
    PostgresNotifier(ruvia::DbConfig config, std::function<void()> wake);
    ~PostgresNotifier();

    PostgresNotifier(const PostgresNotifier&) = delete;
    PostgresNotifier& operator=(const PostgresNotifier&) = delete;

    void start();
    void stop();
    [[nodiscard]] bool connected() const noexcept;

private:
    void run(std::stop_token stopToken);

    ruvia::DbConfig config_;
    std::function<void()> wake_;
    std::atomic_bool connected_{false};
    std::mutex lifecycleMutex_;
    std::jthread thread_;
};

} // namespace service::message::outbox
