#pragma once

#include <functional>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include <ruvia/web/Context.h>

namespace service::middleware {

// Snapshot DTOs must not accumulate in a long-lived HTTP request arena.
// This allocation scope and every I/O operation stay on the accepting Worker.
class RequestContext {
  public:
    RequestContext(ruvia::Context& connection, std::string actorId)
        : RequestContext(connection, std::move(actorId), connection.stopToken()) {}

    RequestContext(ruvia::Context& connection, std::string actorId, ruvia::StopToken cancellation)
        : userId(std::move(actorId)), stop(std::move(cancellation)), connection_(connection), memory_(connection.pool()) {}

    auto db() const { return connection_.db().withOptions({ .stopToken = stop }); }

    auto redis() const { return connection_.redis().withOptions({ .stopToken = stop }); }

    auto pool() const { return connection_.pool(); }

    auto arena() { return &memory_; }

    auto httpClient(std::string_view alias) const { return connection_.httpClient(alias).withOptions({ .stopToken = stop }); }

    auto worker() const { return connection_.worker(); }

    template <typename State>
    State& workerState() const { return connection_.workerState<State>(); }

    auto stopToken() const { return stop; }

    const auto& env() const { return connection_.env(); }

    std::string userId;
    ruvia::StopToken stop;

  private:
    ruvia::Context& connection_;
    std::pmr::monotonic_buffer_resource memory_;
};

// Bound by authentication middleware for this request only. The query runs
// on the connection's Worker and owns no cross-request mutable state.
struct AuthenticatedUserSnapshot {
    std::function<ruvia::Task<std::string>(RequestContext&)> query;
};

} // namespace service::middleware
