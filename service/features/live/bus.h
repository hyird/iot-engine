#pragma once

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <ruvia/core/Channel.h>

namespace service::live {

// Notifications carry no data or authorization decisions. A subscriber always
// rebuilds its own authorized snapshot. Capacity one intentionally coalesces
// intermediate changes while a slow subscriber is writing its current snapshot.
class Bus final {
  public:
    struct Subscription {
        std::string topic;
        ruvia::ChannelSender<int> sender;
        ruvia::ChannelReceiver<int> receiver;
    };

    std::shared_ptr<Subscription> subscribe(const ruvia::WorkerHandle& worker,
                                           std::string_view topic) {
        auto [sender, receiver] = ruvia::makeChannel<int>(worker, {.capacity = 1});
        auto subscription = std::make_shared<Subscription>(
            Subscription{std::string(topic), std::move(sender), std::move(receiver)});
        std::lock_guard lock(mutex_);
        std::erase_if(subscriptions_, [](const auto& entry) { return entry.expired(); });
        subscriptions_.push_back(subscription);
        return subscription;
    }

    void publish(std::string_view topic) {
        std::lock_guard lock(mutex_);
        std::erase_if(subscriptions_, [&](const auto& entry) {
            auto subscription = entry.lock();
            if (!subscription) return true;
            const auto& target = subscription->topic;
            const bool related =
                (target == "device" && (topic == "protocol" || topic == "link" || topic == "edge" || topic == "command")) ||
                (target == "access" && (topic == "device" || topic == "protocol" || topic == "alert")) ||
                (target == "edge" && (topic == "device" || topic == "link" || topic == "protocol")) ||
                (target == "alert" && (topic == "protocol" || topic == "device")) ||
                (target == "vpn" && topic == "edge");
            if (topic == "*" || topic == "auth" || target == topic || related)
                (void)subscription->sender.send(1);
            return false;
        });
    }

  private:
    std::mutex mutex_;
    std::vector<std::weak_ptr<Subscription>> subscriptions_;
};

inline Bus& bus() {
    static Bus value;
    return value;
}

} // namespace service::live
