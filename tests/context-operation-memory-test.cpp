#include <ruvia/core/EventLoopPool.h>
#include <ruvia/core/memory/MemoryPool.h>
#include <ruvia/web/App.h>
#include <ruvia/web/Controller.h>
#include <ruvia/web/HttpClient.h>
#include <ruvia/web/db/DbClient.h>
#include <ruvia/web/db/DbQuery.h>
#include <ruvia/web/redis/RedisClient.h>

#include "service/middleware/request_context.h"
#include "tests/redis_reply.fixture.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory_resource>
#include <span>
#include <string>
#include <thread>

namespace {

RUVIA_MODEL(OperationMemoryResult, RUVIA_REQUIRED_FIELD(value, ruvia::String));

void check(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "%s\n", message);
        std::abort();
    }
}

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t live() const noexcept { return live_; }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        auto* value = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        ++live_;
        return value;
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        --live_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t live_{};
};

bool adjacent(const void* first, const void* second) {
    const auto left = reinterpret_cast<std::uintptr_t>(first);
    const auto right = reinterpret_cast<std::uintptr_t>(second);
    return left + 1 == right || right + 1 == left;
}

// Minimal PostgreSQL peer for public-client ownership checks. It completes the
// startup and extended query exchange and always returns one owned text field.
class PostgreSqlReplyServer final {
public:
    PostgreSqlReplyServer()
        : acceptor_(io_, {asio::ip::address_v4::loopback(), 0}),
          port_(acceptor_.local_endpoint().port()), thread_([this] { serve(); }) {}
    ~PostgreSqlReplyServer() {
        asio::error_code error;
        asio::ip::tcp::socket wake(io_);
        wake.connect({asio::ip::address_v4::loopback(), port_}, error);
        wake.close(error);
        thread_.join();
    }
    std::uint16_t port() const noexcept { return port_; }

private:
    static std::string number(std::uint32_t value, unsigned bytes = 4) {
        std::string output(bytes, '\0');
        for (unsigned index = 0; index < bytes; ++index)
            output[index] = static_cast<char>(value >> (8 * (bytes - index - 1)));
        return output;
    }
    static std::uint32_t length(const std::array<unsigned char, 4>& value) {
        return (std::uint32_t(value[0]) << 24) | (std::uint32_t(value[1]) << 16) |
            (std::uint32_t(value[2]) << 8) | value[3];
    }
    void serve() noexcept {
        try {
            asio::ip::tcp::socket socket(io_);
            acceptor_.accept(socket);
            const auto payload = [&] {
                std::array<unsigned char, 4> size{};
                asio::read(socket, asio::buffer(size));
                const auto count = length(size);
                if (count < 4 || count > 1024 * 1024) throw std::runtime_error("invalid PostgreSQL fixture frame");
                std::string body(count - 4, '\0');
                asio::read(socket, asio::buffer(body));
                return body;
            };
            const auto send = [&](char type, std::string_view body) {
                const auto frame = std::string(1, type) + number(static_cast<std::uint32_t>(body.size() + 4)) + std::string(body);
                asio::write(socket, asio::buffer(frame));
            };
            (void)payload();
            send('R', number(0));
            send('S', std::string("client_encoding\0UTF8\0", 21));
            send('S', std::string("server_version\00018.0\0", 20));
            send('K', number(1) + number(2));
            send('Z', "I");
            for (;;) {
                char type{};
                asio::read(socket, asio::buffer(&type, 1));
                (void)payload();
                if (type == 'X') return;
                if (type == 'P') send('1', {});
                if (type == 'B') send('2', {});
                if (type == 'D' || type == 'Q') {
                    send('T', number(1, 2) + std::string("value\0", 6) + number(0) +
                        number(0, 2) + number(25) + number(0xffff, 2) + number(0xffffffff) + number(0, 2));
                }
                if (type == 'E' || type == 'Q') {
                    send('D', number(1, 2) + number(2048) + std::string(2048, 'r'));
                    send('C', std::string("SELECT 1\0", 9));
                }
                if (type == 'S' || type == 'Q') send('Z', "I");
            }
        } catch (...) {
            // Client closure ends this fixture; protocol failures reach its task.
        }
    }

    asio::io_context io_;
    asio::ip::tcp::acceptor acceptor_;
    std::uint16_t port_;
    std::thread thread_;
};

ruvia::Task<void> coldOperationsUseClientStorage(ruvia::EventLoop loop,
    std::uint16_t databasePort, std::uint16_t redisPort) {
    ruvia::WorkerMemory worker;
    std::array<std::byte, 64 * 1024> buffer{};
    ruvia::RequestMemory request(worker, buffer);
    check(request.upstreamResource() == worker.resource(),
        "request memory does not borrow its worker pool");

    ruvia::DbConfig databaseConfig;
    databaseConfig.driver = ruvia::DbDriver::kPostgreSql;
    databaseConfig.tls.mode = ruvia::client_tls_mode::disabled;
    databaseConfig.port = databasePort;
    databaseConfig.connectTimeout = std::chrono::seconds(2);
    databaseConfig.queryTimeout = std::chrono::seconds(2);
    ruvia::DbClient database(loop, databaseConfig);
    ruvia::RedisConfig redisConfig;
    redisConfig.tls.mode = ruvia::client_tls_mode::disabled;
    redisConfig.port = redisPort;
    redisConfig.poolSizePerWorker = 1;
    ruvia::RedisClient redis(loop, redisConfig);
    co_await database.connect();
    co_await redis.connect();
    for (int index = 0; index != 2000; ++index) {
        const auto* before = request.resource()->allocate(1, 1);
        {
            auto operation = database.query("SELECT ?", std::string(2048, char('a' + index % 26)));
            (void)operation;
        }
        {
            auto operation = redis.command("SET", "key", std::string(2048, char('a' + index % 26)));
            (void)operation;
        }
        const auto* after = request.resource()->allocate(1, 1);
        check(adjacent(before, after), "long lived operation advanced request arena");
    }
    const auto* before = request.resource()->allocate(1, 1);
    (void)request.resource()->allocate(2048, 1);
    const auto* after = request.resource()->allocate(1, 1);
    check(!adjacent(before, after), "arena probe missed an injected request allocation");
    {
        const auto retained = co_await database.query("SELECT $1", std::string(2048, 'r'));
        check(retained.front()["value"].value() == std::string(2048, 'r'),
            "retained DB result changed");
        for (int index = 0; index != 100; ++index) {
            const auto transient = co_await database.query("SELECT $1", std::string(2048, 'x'));
            check(transient.front()[0].value()->size() == 2048, "DB result value mismatch");
            check(retained.front()["value"].value() == std::string(2048, 'r'),
                "later DB query invalidated a retained result");
        }
    }
    co_await redis.shutdown();
    co_await database.shutdown();
}

ruvia::DbStatement ownedStatement(std::pmr::memory_resource* resource, char fill) {
    ruvia::DbQuery query(resource);
    query.select(query.value(std::string(2048, fill)));
    return query.compile(ruvia::DbDriver::kPostgreSql, resource, ruvia::DbParameterMode::kLiteral);
}

void resultStorageIsReclaimed() {
    {
        CountingResource resource;
        {
            const auto retained = ownedStatement(&resource, 'r');
            const auto expected = std::string(retained.sql());
            const auto baseline = resource.live();
            check(expected.find(std::string(2048, 'r')) != std::string::npos,
                "compiled DB result lost owned input");
            for (int index = 0; index != 100; ++index) {
                {
                    const auto transient = ownedStatement(&resource, char('a' + index % 26));
                    check(resource.live() > baseline, "DB statement did not allocate");
                    check(!transient.sql().empty(), "DB statement result is empty");
                }
                check(resource.live() == baseline, "DB statement storage accumulated");
                check(retained.sql() == expected, "retained DB result changed");
            }
        }
        check(resource.live() == 0, "DB statement storage was not released");
    }
    {
        CountingResource resource;
        {
            const auto retained = test::redisString("retained", &resource);
            const auto baseline = resource.live();
            for (int index = 0; index != 100; ++index) {
                {
                    const auto transient = test::redisString(
                        std::string(2048, char('a' + index % 26)), &resource);
                    check(transient.string().size() == 2048, "Redis result value mismatch");
                }
                check(resource.live() == baseline, "Redis result storage accumulated");
            }
            check(retained.string() == "retained", "retained Redis result changed");
        }
        check(resource.live() == 0, "Redis result storage was not released");
    }
}

class OperationMemoryController final : public ruvia::Controller<OperationMemoryController> {
public:
    RUVIA_CONTROLLER_GROUP("/operation-memory")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("", probe);
    RUVIA_ROUTES_END

private:
    ruvia::Task<ruvia::HttpResponse> probe(ruvia::Context& connection) {
        for (int index = 0; index != 2000; ++index) {
            const auto* before = connection.arena()->allocate(1, 1);
            {
                auto operation = connection.db().query("SELECT ?", std::string(2048, 'd'));
                (void)operation;
            }
            {
                auto operation = connection.redis().command("SET", "key", std::string(2048, 'r'));
                (void)operation;
            }
            {
                service::middleware::RequestContext request(connection, "memory-test");
                check(request.pool() == connection.pool(), "operation pool is not worker owned");
                (void)request.arena()->allocate(2048, 1);
            }
            const auto* after = connection.arena()->allocate(1, 1);
            check(adjacent(before, after), "operation storage advanced the HTTP request arena");
        }
        const auto* before = connection.arena()->allocate(1, 1);
        (void)connection.arena()->allocate(2048, 1);
        const auto* after = connection.arena()->allocate(1, 1);
        check(!adjacent(before, after), "HTTP arena probe missed an injected request allocation");
        service::middleware::RequestContext request(connection, "memory-test");
        OperationMemoryResult result({.resource = request.arena()});
        result.set<"value">("operation storage released");
        co_return connection.json(result);
    }
};

ruvia::Task<void> verifyHttpRequest(ruvia::EventLoop loop, std::uint16_t port) {
    ruvia::HttpClient client(loop, {.scheme = ruvia::HttpScheme::kHttp,
        .host = "127.0.0.1", .port = port, .requestTimeout = std::chrono::seconds(5)});
    {
        auto response = co_await client.send({.target = "/operation-memory"});
        const auto body = co_await response.body().readAll();
        check(response.status() == ruvia::http_status::kOk &&
            std::string_view(reinterpret_cast<const char*>(body.bytes().data()), body.size()) ==
                R"({"value":"operation storage released"})",
            "serialized result did not survive operation storage release");
    }
    co_await client.shutdown();
}

void requestStorageSurvives() {
    PostgreSqlReplyServer database;
    test::RedisReplyServer redis("+PONG\r\n");
    ruvia::DbConfig databaseConfig;
    databaseConfig.driver = ruvia::DbDriver::kPostgreSql;
    databaseConfig.port = database.port();
    databaseConfig.tls.mode = ruvia::client_tls_mode::disabled;
    ruvia::RedisConfig redisConfig;
    redisConfig.port = redis.port();
    redisConfig.poolSizePerWorker = 1;
    redisConfig.tls.mode = ruvia::client_tls_mode::disabled;
    asio::io_context io;
    asio::ip::tcp::acceptor reservation(io, {asio::ip::address_v4::loopback(), 0});
    const auto port = reservation.local_endpoint().port();
    reservation.close();
    auto& app = ruvia::app();
    std::promise<void> ready;
    auto started = ready.get_future();
    app.blockingPool(nullptr).listen({.address = "127.0.0.1", .http = port})
        .server({.worker_count = 1}).database({.config = databaseConfig})
        .redis({.config = redisConfig}).onStart([&] { ready.set_value(); });
    std::exception_ptr serverError;
    std::thread serving([&] {
        try {
            app.run();
        } catch (...) {
            serverError = std::current_exception();
            try { ready.set_exception(serverError); } catch (...) {}
        }
    });
    try {
        if (started.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
            throw std::runtime_error("operation memory app startup timed out");
        started.get();
        ruvia::EventLoopPool clients({.loopCount = 1});
        clients.start();
        clients.loop(0).start(verifyHttpRequest(clients.loop(0), port)).get();
        clients.stop();
        clients.join();
    } catch (...) {
        app.stop();
        serving.join();
        throw;
    }
    app.stop();
    serving.join();
    if (serverError) std::rethrow_exception(serverError);
}

} // namespace

int main() {
    try {
        ruvia::EventLoopPool pool({.loopCount = 1});
        PostgreSqlReplyServer database;
        test::RedisReplyServer redis("+PONG\r\n");
        pool.start();
        pool.loop(0).start(coldOperationsUseClientStorage(pool.loop(0), database.port(), redis.port())).get();
        pool.stop();
        pool.join();
        resultStorageIsReclaimed();
        requestStorageSurvives();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
