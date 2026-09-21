#include <array>
#include <asio/ip/tcp.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <ruvia/core/EventLoopPool.h>
#include <ruvia/web/App.h>
#include <ruvia/web/HttpClient.h>

#include <ruvia/web/Controller.h>
#include "service/modules/link/link.service.h"

using namespace std::chrono_literals;

namespace {
void require(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

class Origin final {
  public:
    Origin() : acceptor_(io_, { asio::ip::address_v4::loopback(), 0 }) {
        acceptor_.non_blocking(true);
        thread_ = std::jthread([this](std::stop_token stop) {
            run(stop);
        });
    }

    ~Origin() { thread_.request_stop(); }

    std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

    std::atomic_int scenario{0};
    std::atomic_int requests{0};
    std::atomic_bool failed{ false };

  private:
    bool send(asio::ip::tcp::socket& socket, std::string_view bytes, std::stop_token stop) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!bytes.empty() && !stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::error_code error;
            const auto size = socket.write_some(asio::buffer(bytes.data(), bytes.size()), error);
            if (error == asio::error::would_block || error == asio::error::try_again) {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            if (error) {
                return false;
            }
            bytes.remove_prefix(size);
        }
        return bytes.empty();
    }

    void run(std::stop_token stop) {
        try {
            while (!stop.stop_requested()) {
                asio::ip::tcp::socket socket(io_);
                std::error_code error;
                acceptor_.accept(socket, error);
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    std::this_thread::sleep_for(2ms);
                    continue;
                }
                if (error) {
                    throw std::system_error(error);
                }
                socket.non_blocking(true);
                std::string request;
                std::array<char, 4096> buffer{};
                const auto deadline = std::chrono::steady_clock::now() + 5s;
                while (request.find("\r\n\r\n") == std::string::npos && !stop.stop_requested()) {
                    require(std::chrono::steady_clock::now() < deadline, "origin request timed out");
                    const auto size = socket.read_some(asio::buffer(buffer), error);
                    if (error == asio::error::would_block || error == asio::error::try_again) {
                        std::this_thread::sleep_for(2ms);
                        continue;
                    }
                    if (error) {
                        throw std::system_error(error);
                    }
                    request.append(buffer.data(), size);
                    require(request.size() < 65536, "origin request exceeded limit");
                }
                ++requests;
                require(request.starts_with("GET / HTTP/1.1\r\n"), "public IP request target changed");
                require(request.find("text/plain") != std::string::npos, "missing text Accept header");
                const int selected = scenario.load();
                if (selected == 5) continue;
                if (selected == 4) {
                    (void)send(socket, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\nB\r\n2001:db8::1\r\n0\r\n\r\n", stop);
                    continue;
                }
                const std::string body = selected == 0 ? "  203.0.113.7\r\n" :
                    selected == 1 ? "203.0.113.8" : selected == 2 ? "<html>error</html>" :
                    selected == 3 ? " \r\n" : std::string(65537, 'a');
                const auto status = selected == 1 ? "503 Unavailable" : "200 OK";
                (void)send(socket, std::string("HTTP/1.1 ") + status + "\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body, stop);
            }
        } catch (...) {
            failed.store(true);
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::jthread thread_;
};

} // namespace

class PublicIpTestController final : public ruvia::Controller<PublicIpTestController> {
public:
    RUVIA_CONTROLLER_GROUP("/test")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/ip", ip);
    RUVIA_ROUTES_END
private:
    ruvia::Task<ruvia::HttpResponse> ip(ruvia::Context& context) {
        const auto owner = std::this_thread::get_id();
        service::link::LinkService service;
        const auto first = co_await service.publicIp(context);
        if (!first.empty()) {
            require(co_await service.publicIp(context) == first, "cached IP changed");
        }
        require(owner == std::this_thread::get_id(), "public IP query left its Worker");
        co_return context.text(std::string_view(first));
    }
};

ruvia::Task<void> verifyPublicIp(ruvia::EventLoop loop, std::uint16_t port, Origin& origin) {
    ruvia::HttpClient client(loop, {.scheme = ruvia::HttpScheme::kHttp, .host = "127.0.0.1", .port = port, .requestTimeout = 5s});
    for (int scenario = 0; scenario != 7; ++scenario) {
        origin.scenario = scenario;
        const auto before = origin.requests.load();
        auto response = co_await client.send({.target = "/test/ip"});
        require(response.status().value() == 200, "public IP handler failed");
        const auto body = co_await response.body().readAll();
        const std::string_view expected = scenario == 0 ? "203.0.113.7" : scenario == 4 ? "2001:db8::1" : "";
        require(std::string_view(reinterpret_cast<const char*>(body.data()), body.size()) == expected, "public IP response validation failed");
        require(origin.requests.load() == before + 1, "cached query reached upstream again");
    }
    co_await client.shutdown();
}

int main() {
    auto& app = ruvia::app();
    std::thread serving;
    std::exception_ptr serverError;
    try {
        Origin origin;
        asio::io_context reserveIo;
        asio::ip::tcp::acceptor reservation(reserveIo, {asio::ip::address_v4::loopback(), 0});
        const auto port = reservation.local_endpoint().port();
        reservation.close();
        std::promise<void> ready;
        auto started = ready.get_future();
        app.blockingPool(nullptr).listen({.address = "127.0.0.1", .http = port})
            .server({.workerCount = 2})
            .httpClient({.alias = "link-public-ip", .config = {
                .scheme = ruvia::HttpScheme::kHttp, .host = "127.0.0.1", .port = origin.port(),
                .requestTimeout = 2s, .maxResponseBytes = 64U * 1024U,
                .protocol = ruvia::HttpClientProtocol::kHttp1Only, .userAgent = "curl/8.0"}})
            .onStart([&] { ready.set_value(); });
        serving = std::thread([&] {
            try { app.run(); }
            catch (...) {
                serverError = std::current_exception();
                try { ready.set_exception(serverError); } catch (...) {}
            }
        });
        require(started.wait_for(10s) == std::future_status::ready, "test app startup timed out");
        started.get();
        ruvia::EventLoopPool pool({.loopCount = 1});
        pool.start();
        pool.loop(0).start(verifyPublicIp(pool.loop(0), port, origin)).get();
        pool.stop();
        pool.join();
        require(!origin.failed.load(), "local HTTP origin failed");
        app.stop();
        serving.join();
        if (serverError) std::rethrow_exception(serverError);
        std::cout << "public IP HTTP validation, cache and Worker tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        app.stop();
        if (serving.joinable()) serving.join();
        std::cerr << error.what() << '\n';
        return 1;
    }
}
