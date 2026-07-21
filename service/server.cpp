#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#include <ruvia/web/App.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbMigration.h>
#include <ruvia/web/redis/Redis.h>

#include "service/common/http.h"
#include "service/config/schema.h"
#include "service/modules/iot/device/device.controller.h"
#include "service/modules/iot/device-group/device-group.controller.h"
#include "service/modules/iot/link/link.controller.h"
#include "service/modules/iot/protocol/protocol.controller.h"
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
    config.poolSizePerWorker = env.get<std::uint32_t>("REDIS_POOL_SIZE_PER_WORKER").value_or(2);
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

} // namespace

int main(int argc, char* argv[]) {
    try {
        auto& app = ruvia::app();
        app.loadDotenv();

        auto db = databaseConfig(app.env());
        ruvia::DbMigrationOptions migrationOptions;
        migrationOptions.table = "iot_engine_schema_migrations";
        const auto report = ruvia::DbMigrator::migrate(db, service::config::kSchemaMigrations,
                                                       std::move(migrationOptions));
        std::cout << "database migrations: applied=" << report.applied().size()
                  << ", skipped=" << report.skipped().size() << '\n';

        configureWeb(app, runtimeDirectory(argc > 0 ? argv[0] : nullptr));
        app.useDb(std::move(db))
            .useRedis(redisConfig(app.env()))
            .onError(&handleError)
            .setListenAddress(app.env().get("HOST").value_or("0.0.0.0"))
            .setServerTopology(
                ruvia::ServerTopology::http(app.env().get<std::uint16_t>("PORT").value_or(1102)))
            .setWorkersPerListener(app.env().get<std::uint32_t>("WORKER_THREADS").value_or(2))
            .run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "server failed: " << error.what() << '\n';
        return 1;
    }
}
