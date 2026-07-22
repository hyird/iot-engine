#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <ruvia/web/App.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbMigration.h>
#include <ruvia/web/redis/Redis.h>

#include "service/common/http.h"
#include "service/common/packet_log.h"
#include "service/config/schema.h"
#include "service/modules/northbridge/device/device.controller.h"
#include "service/modules/northbridge/link/link.controller.h"
#include "service/modules/northbridge/open/open_access.controller.h"
#include "service/modules/northbridge/open/open_webhook.runtime.h"
#include "service/modules/northbridge/command/command_result.runtime.h"
#include "service/modules/northbridge/command/protocol_command.queue.h"
#include "service/modules/northbridge/config/runtime_config.projector.h"
#include "service/modules/northbridge/config/runtime_config.reconciler.h"
#include "service/modules/northbridge/protocol/protocol.controller.h"
#include "service/modules/northbridge/telemetry/telemetry_persistence.runtime.h"
#include "service/modules/northbridge/telemetry/device_latest.redis.h"
#include "service/modules/southbridge/southbridge.pool.h"
#include "service/modules/system/auth/auth.controller.h"
#include "service/modules/system/dept/dept.controller.h"
#include "service/modules/system/role/role.controller.h"
#include "service/modules/system/user/user.controller.h"

namespace {

void assign(std::pmr::string& target, std::optional<std::string_view> value) {
    if (value)
        target.assign(*value);
}

ruvia::DbConfig databaseConfig(const ruvia::Env& env) {
    auto config = ruvia::DbConfig::postgreSql();
    assign(config.host, env.get("DB_HOST"));
    assign(config.username, env.get("DB_USERNAME"));
    assign(config.password, env.get("DB_PASSWORD"));
    assign(config.database, env.get("DB_DATABASE"));
    config.port = env.get<std::uint16_t>("DB_PORT").value_or(5432);
    config.acquireTimeout = std::chrono::seconds(2);
    config.connectTimeout = std::chrono::seconds(5);
    config.queryTimeout = std::chrono::seconds(30);
    return config;
}

ruvia::RedisConfig redisConfig(const ruvia::Env& env) {
    ruvia::RedisConfig config;
    assign(config.host, env.get("REDIS_HOST"));
    assign(config.password, env.get("REDIS_PASSWORD"));
    config.port = env.get<std::uint16_t>("REDIS_PORT").value_or(6379);
    config.database = env.get<std::uint32_t>("REDIS_DATABASE").value_or(0);
    config.poolSizePerWorker = 1;
    return config;
}

std::filesystem::path runtimeDirectory(const char* executable) {
    if (!executable || *executable == '\0')
        return std::filesystem::current_path();
    std::error_code error;
    auto path = std::filesystem::weakly_canonical(std::filesystem::absolute(executable), error);
    if (error)
        path = std::filesystem::absolute(executable);
    return path.parent_path();
}

service::common::packet_log::Config packetLogConfig(const ruvia::Env& env,
                                                    const std::filesystem::path& runtime) {
    service::common::packet_log::Config config;
    config.directory = runtime / "logs";
    config.level =
        service::common::packet_log::parseLevel(env.get("PACKET_LOG_LEVEL").value_or("DEBUG"));
    return config;
}

void configureWeb(ruvia::App& app, const std::filesystem::path& runtime) {
    const auto webRoot = runtime / "web";
    if (!std::filesystem::is_directory(webRoot))
        return;
    ruvia::DocumentRootConfig config;
    config.root = webRoot;
    config.staticOptions.indexFile = "index.html";
    config.staticOptions.cacheControl = "no-cache";
    app.setDocumentRoot(std::move(config));
}

ruvia::Task<ruvia::HttpResponse> handleError(ruvia::Context& c, ruvia::HttpErrorInfo info) {
    c.status(info.status());
    const auto message = info.message().empty() ? std::string_view("请求失败") : info.message();
    co_return c.json(service::common::error(
        c, service::common::errorCode(info.code(), info.status().value()), message));
}

ruvia::Task<void>
startSouthbridge(ruvia::WebWorkerContext& context,
                 std::shared_ptr<service::southbridge::SouthbridgeRuntime> southbridge,
                 ruvia::RedisConfig redis, std::size_t workerCount,
                 std::shared_ptr<std::promise<void>> started) {
    try {
        (void)co_await service::northbridge::config::projectRuntimeConfig(context);
        co_await service::northbridge::telemetry::latest::hydrate(context);
        southbridge->start(std::move(redis), workerCount);
        started->set_value();
    } catch (...) {
        try {
            started->set_exception(std::current_exception());
        } catch (...) {
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        ruvia::App app;
        app.loadDotenv();
        const auto runtime = runtimeDirectory(argc > 0 ? argv[0] : nullptr);
        service::common::packet_log::initialize(packetLogConfig(app.env(), runtime));

        auto db = databaseConfig(app.env());
        ruvia::DbMigrationOptions migrationOptions;
        migrationOptions.table = "sys_schema_migrations";
        const auto report = ruvia::DbMigrator::migrate(db, service::config::kSchemaMigrations,
                                                       std::move(migrationOptions));
        std::cout << "database migrations: applied=" << report.applied().size()
                  << ", skipped=" << report.skipped().size() << '\n';

        configureWeb(app, runtime);
        app.useController<service::auth::AuthController>()
            .useController<service::dept::DeptController>()
            .useController<service::role::RoleController>()
            .useController<service::user::UserController>()
            .useController<service::link::LinkController>()
            .useController<service::protocol::ProtocolController>()
            .useController<service::device::DeviceController>()
            .useController<service::open_access::OpenAccessAdminController>()
            .useController<service::open_access::OpenApiController>();
        const auto cpu = std::max(2U, std::thread::hardware_concurrency());
        const auto northWorkerCount = static_cast<std::size_t>((cpu + 1U) / 2U);
        const auto southWorkerCount = static_cast<std::size_t>(cpu / 2U);
        auto northRedis = redisConfig(app.env());
        auto southRedis = northRedis;
        auto southbridge = std::make_shared<service::southbridge::SouthbridgeRuntime>();
        auto telemetry =
            std::make_shared<service::northbridge::telemetry::TelemetryPersistenceRuntime>();
        auto commandResults =
            std::make_shared<service::northbridge::command::CommandResultRuntime>();
        auto openWebhooks = std::make_shared<service::open_access::OpenWebhookRuntime>();
        auto configReconciler =
            std::make_shared<service::northbridge::config::RuntimeConfigReconciler>();
        app.useDb(std::move(db))
            .useRedis(std::move(northRedis))
            .onStart([southbridge, telemetry, commandResults, openWebhooks, configReconciler,
                      southRedis = std::move(southRedis), southWorkerCount, &app]() mutable {
                auto workers = app.workers();
                if (workers.empty())
                    throw std::runtime_error(
                        "northbridge: no worker available for config projection");
                telemetry->start(workers, southWorkerCount);
                commandResults->start(workers, southWorkerCount);
                openWebhooks->start(workers);
                auto started = std::make_shared<std::promise<void>>();
                auto ready = started->get_future();
                const auto posted = workers.front().post(
                    [southbridge, southRedis, southWorkerCount,
                     started](ruvia::WebWorkerContext& context) mutable -> ruvia::Task<void> {
                        return startSouthbridge(context, southbridge, std::move(southRedis),
                                                southWorkerCount, started);
                    });
                if (!posted.accepted())
                    throw std::runtime_error("northbridge rejected runtime config projection");
                ready.get();
                configReconciler->start(workers.front(), southWorkerCount);
            })
            .onStop([southbridge, telemetry, commandResults, openWebhooks, configReconciler] {
                configReconciler->stop();
                southbridge->stop();
                telemetry->stop();
                commandResults->stop();
                openWebhooks->stop();
            })
            .onError(&handleError)
            .setListenAddress(app.env().get("HOST").value_or("0.0.0.0"))
            .setServerTopology(
                ruvia::ServerTopology::http(app.env().get<std::uint16_t>("PORT").value_or(1102)))
            .setWorkersPerListener(northWorkerCount)
            .run();
        service::common::packet_log::shutdown();
        return 0;
    } catch (const std::exception& error) {
        service::common::packet_log::shutdown();
        std::cerr << "server failed: " << error.what() << '\n';
        return 1;
    }
}
