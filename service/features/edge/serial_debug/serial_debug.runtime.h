#pragma once
#include <exception>
#include <ruvia/core/TaskScope.h>
#include "service/features/edge/serial_debug/serial_debug.service.h"

namespace service::edge::serial_debug {
class Runtime final {
  public:
    static ruvia::Task<void> serve(ruvia::Context& c, std::string_view token) {
        auto& socket = c.webSocket();
        const auto ticket = co_await Service::consumeTicket(c, token);
        const auto id = service::common::nextUuidV7();
        if (!ticket || !co_await Service::registerSession(c, *ticket, id)) {
            co_await socket.close({.code = 1008, .reason = "serial ticket expired or node reconnected"});
            co_return;
        }
        BrowserSession state;
        ruvia::TaskScope scope(c.worker(), ruvia::TaskScopeOptions{.resource = c.pool()});
        std::exception_ptr failure;
        try {
            wire::SerialDebugRequest open;
            open.set_action("open");
            open.set_request_sequence(1);
            open.mutable_settings()->set_channel(ticket->path);
            if (!co_await Service::enqueue(c, *ticket, id, std::move(open)))
                throw std::runtime_error("serial open queue unavailable");
            scope.spawn(pump(c, *ticket, id, state, scope.stopToken()));
            while (auto message = co_await socket.read()) {
                if (message->binary() || !state.opened || state.closed) break;
                auto request = decodeRequest(message->payload(), ticket->path, state.requestSequence);
                if (!request) break;
                state.requestSequence = request->request_sequence();
                state.lastInput = std::chrono::steady_clock::now();
                if (!co_await Service::enqueue(c, *ticket, id, std::move(*request))) break;
            }
        } catch (...) { failure = std::current_exception(); }
        scope.requestStop();
        try { co_await scope.join(); } catch (...) { if (!failure) failure = std::current_exception(); }
        try {
            wire::SerialDebugRequest close;
            close.set_action("close");
            close.set_request_sequence(++state.requestSequence);
            close.mutable_settings()->set_channel(ticket->path);
            (void)co_await Service::enqueue(c, *ticket, id, std::move(close));
            co_await Service::release(c, *ticket, id);
        } catch (...) { if (!failure) failure = std::current_exception(); }
        if (failure) {
            socket.abort();
            std::rethrow_exception(failure);
        }
        co_await socket.close({.code = 1000, .reason = "serial debug closed"});
    }
  private:
    static ruvia::Task<void> pump(ruvia::Context& c, TicketRecord ticket, std::string id,
        BrowserSession& state, ruvia::StopToken stop) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        auto nextCheck = std::chrono::steady_clock::now();
        try {
            while (!stop.stopRequested()) {
                const auto item = co_await Service::takeOutput(c, ticket, id);
                if (item) {
                    const auto parsed = ruvia::JsonValue::parse(*item);
                    const auto kind = parsed ? service::utils::jsonField(*parsed, "kind") : std::nullopt;
                    // JSON comes from eventJson; exact literals avoid another DTO allocation.
                    if (kind && kind->view() == "\"state\"") state.opened = true;
                    if (kind && kind->view() == "\"closed\"") state.closed = true;
                    co_await c.webSocket().text(*item);
                    if (state.closed) { c.webSocket().abort(); co_return; }
                }
                const auto now = std::chrono::steady_clock::now();
                if ((!state.opened && now >= deadline) ||
                    now - state.lastInput > std::chrono::seconds(40)) {
                    co_await c.webSocket().text("{\"kind\":\"closed\",\"message\":\"串口调试会话超时，请重新打开\"}");
                    c.webSocket().abort(); co_return;
                }
                if (now >= nextCheck) {
                    if (!co_await Service::connected(c, ticket)) {
                        co_await c.webSocket().text("{\"kind\":\"closed\",\"message\":\"节点已断开或重新连接\"}");
                        c.webSocket().abort(); co_return;
                    }
                    nextCheck = now + std::chrono::seconds(3);
                }
                if (!item) (void)co_await ruvia::sleepFor(c.worker(), std::chrono::milliseconds(20), stop);
            }
        } catch (...) {
            c.webSocket().abort();
            throw;
        }
    }
};
} // namespace service::edge::serial_debug
