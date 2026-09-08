#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <ruvia/web/Controller.h>
#include <ruvia/web/ModelJson.h>
#include "service/common/http.h"
#include "service/features/live/bus.h"
#include "service/middleware/auth.h"

namespace service::live {

template <typename Model>
std::string json(const Model& model) {
    return std::string(ruvia::toJson(model));
}

inline std::string data(ruvia::Context&, std::string_view value) {
    return "{\"code\":0,\"message\":\"ok\",\"data\":" + std::string(value) + "}";
}

// The query callback includes authentication and authorization. It runs before
// opening the stream and before every snapshot; no data is broadcast by the bus.
template <typename Query>
ruvia::Task<void> serve(ruvia::Context& context, std::string_view topic, Query query,
                      std::function<ruvia::Task<void>()> authorize = {}) {
    using namespace std::chrono_literals;
    if (context.req().header("Accept").value_or("").find("text/event-stream") ==
        std::string_view::npos)
        service::common::fail(10002, "This query requires text/event-stream", 406);
    auto subscription = bus().subscribe(context.worker(), topic);
    auto snapshot = co_await query();
    context.header("X-Accel-Buffering", "no");
    context.header("Cache-Control", "no-store");
    auto stream = context.streamSse();
    std::uint64_t revision = 1;
    auto id = std::to_string(revision);
    co_await stream.write({.data = snapshot, .event = "snapshot", .id = id, .retry = 1s});
    // Bound request-arena retention. Reconnection creates a new authorized
    // snapshot; event IDs are connection-local, never misleading replay cursors.
    const auto expires = std::chrono::steady_clock::now() + 5min;
    while (!stream.aborted() && std::chrono::steady_clock::now() < expires) {
        const auto notification = co_await subscription->receiver.receiveFor(
            15s, context.stopToken());
        if (stream.aborted() || context.stopToken().stopRequested()) co_return;
        std::string error;
        std::string next;
        try {
            // Heartbeats only check token expiration. Permission changes publish
            // an auth event, which reexecutes the authorized query immediately.
            if (authorize) co_await authorize();
            else (void)service::middleware::requireAuth(context);
            if (notification.hasValue()) next = co_await query();
        } catch (const ruvia::HttpError& failure) {
            const auto info = failure.info();
            error = json(service::common::error(context,
                service::common::errorCode(info.code(), info.status().value()), info.message()));
        } catch (const std::exception&) {
            error = "{\"code\":10004,\"message\":\"Subscription interrupted\"}";
        }
        if (!error.empty()) {
            co_await stream.write({.data = error, .event = "error"});
            co_return;
        }
        if (notification.hasValue() && next != snapshot) {
            snapshot = std::move(next);
            id = std::to_string(++revision);
            co_await stream.write({.data = snapshot, .event = "snapshot", .id = id});
        } else {
            co_await stream.write({.data = "{}", .event = "heartbeat"});
        }
    }
}

} // namespace service::live
