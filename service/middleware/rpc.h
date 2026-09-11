#pragma once

#include <chrono>
#include <cstddef>
#include <exception>
#include <string>
#include <string_view>

#include "service/common/http.h"
#include "service/common/message.h"
#include "service/common/uuid.h"
#include "service/middleware/live.h"
#include "service/utils/redis.h"

namespace service::rpc {

class RpcRequestClient final {
  public:
    static ruvia::Task<std::string> call(ruvia::Context& context, std::string_view component, std::string_view operation, std::string payload) {
        if (payload.size() > Contract::maximumPayload) {
            service::common::fail(10002, "RPC payload exceeds limit", 413);
        }
        const auto id = service::common::nextUuidV7();
        const auto replyKey = Contract::reply(id);
        const auto deadline = Contract::now() +
            std::chrono::duration_cast<std::chrono::milliseconds>(Contract::timeout).count();
        auto subscription = service::live::bus().subscribe(context.worker(), replyKey);
        const auto requestStream =
            Contract::requests(service::runtime::instanceId(), service::live::bus().workerIndex());
        std::exception_ptr failure;
        try {
            const auto result = co_await service::message::redis::command(context.redis(), { "XADD", requestStream, "*", "version", std::string(Contract::version), "id", id, "component", std::string(component), "operation", std::string(operation), "payload", std::move(payload), "deadline", std::to_string(deadline) });
            if (result.kind() != ruvia::RedisValue::Kind::kString) {
                service::message::redis::throwValue("RPC enqueue", result);
            }
            while (!context.stopToken().stopRequested()) {
                const auto reply = co_await service::message::redis::command(
                    context.redis(),
                    { "GET", replyKey }
                );
                if (reply.kind() == ruvia::RedisValue::Kind::kString) {
                    co_return decodeReply(reply.string());
                }
                if (reply.kind() == ruvia::RedisValue::Kind::kError) {
                    service::message::redis::throwValue("RPC reply", reply);
                }
                const auto remaining = deadline - Contract::now();
                if (remaining <= 0) {
                    service::common::fail(10004, "Background operation timed out", 504);
                }
                co_await subscription->receiver.receiveFor(
                    std::chrono::milliseconds(remaining),
                    context.stopToken()
                );
            }
            service::common::fail(10004, "Background operation cancelled", 503);
        } catch (...) {
            failure = std::current_exception();
        }
        try {
            (void)co_await service::message::redis::command(context.redis(), { "SET", Contract::cancelled(id), "1", "EX", std::string(Contract::replyLifetime) });
        } catch (...) {
            // The persisted deadline also cancels work when the connection is gone.
        }
        std::rethrow_exception(failure);
    }

  private:
    static std::string decodeReply(std::string_view reply) {
        if (reply.starts_with("OK\n")) {
            return std::string(reply.substr(3));
        }
        if (reply.starts_with("ERR\n")) {
            reply.remove_prefix(4);
            const auto statusEnd = reply.find('\n');
            if (statusEnd != std::string_view::npos) {
                const auto status = service::common::parseInt64(reply.substr(0, statusEnd));
                reply.remove_prefix(statusEnd + 1);
                const auto codeEnd = reply.find('\n');
                if (status && *status >= 400 && *status <= 599 && codeEnd != std::string_view::npos) {
                    throw ruvia::HttpError(ruvia::HttpErrorInfoOptions{ .status = ruvia::HttpStatusCode::fromValue(static_cast<std::uint16_t>(*status)), .code = reply.substr(0, codeEnd), .message = reply.substr(codeEnd + 1) });
                }
            }
        }
        service::common::fail(10004, "Invalid background operation response", 502);
    }
};

inline ruvia::Task<std::string> call(ruvia::Context& context, std::string_view component, std::string_view operation, std::string payload) {
    return RpcRequestClient::call(context, component, operation, std::move(payload));
}

} // namespace service::rpc
