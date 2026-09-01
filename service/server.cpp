#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <ruvia/web/App.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/db/DbMigration.h>
#include <ruvia/web/redis/Redis.h>

#include "service/common/http.h"
#include "service/common/packet-log.h"
#include "service/application/runtime.h"
#include "service/config/schema.h"
#include "service/config/storage.h"
#include "service/domains/alert/alert.controller.h"
#include "service/features/alert/runtime.h"
#include "service/domains/gb28181/gb28181.controller.h"
#include "service/domains/gb28181/media.controller.h"
#include "service/features/gb28181/runtime.h"
#include "service/domains/edge/edge.controller.h"
#include "service/features/edge/dispatcher.h"
#include "service/features/edge/gateway.h"
#include "service/features/edge/projector.h"
#include "service/domains/device/device.controller.h"
#include "service/domains/link/link.controller.h"
#include "service/domains/access/access.controller.h"
#include "service/features/access/webhook.h"
#include "service/features/event/outbox.h"
#include "service/features/command/result.h"
#include "service/features/command/queue.h"
#include "service/features/runtime/projector.h"
#include "service/features/runtime/reconciler.h"
#include "service/domains/protocol/protocol.controller.h"
#include "service/features/telemetry/persistence.h"
#include "service/features/telemetry/latest.h"
#include "service/features/collector/runtime.h"
#include "service/domains/auth/auth.controller.h"
#include "service/domains/dept/dept.controller.h"
#include "service/domains/role/role.controller.h"
#include "service/domains/user/user.controller.h"
#include "service/domains/system/operations.controller.h"
#include "service/domains/system/outbox.controller.h"

namespace
{

    template <typename String>
    void assign(String &target, std::optional<std::string_view> value)
    {
        if (value)
            target.assign(*value);
    }

    ruvia::DbConfig databaseConfig(const ruvia::Env &env)
    {
        ruvia::DbConfig config;
        config.driver = ruvia::DbDriver::kPostgreSql;
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

    ruvia::RedisConfig redisConfig(const ruvia::Env &env)
    {
        ruvia::RedisConfig config;
        assign(config.host, env.get("REDIS_HOST"));
        assign(config.password, env.get("REDIS_PASSWORD"));
        config.port = env.get<std::uint16_t>("REDIS_PORT").value_or(6379);
        config.database = env.get<std::uint32_t>("REDIS_DATABASE").value_or(0);
        const auto poolSize = env.get<unsigned>("REDIS_POOL_SIZE_PER_WORKER").value_or(2U);
        if (poolSize == 0U || poolSize > 16U)
            throw std::runtime_error(
                "REDIS_POOL_SIZE_PER_WORKER must be between 1 and 16");
        config.poolSizePerWorker = poolSize;
        return config;
    }

    std::filesystem::path runtimeDirectory(const char *executable)
    {
        if (!executable || *executable == '\0')
            return std::filesystem::current_path();
        std::error_code error;
        auto path = std::filesystem::weakly_canonical(std::filesystem::absolute(executable), error);
        if (error)
            path = std::filesystem::absolute(executable);
        return path.parent_path();
    }

    service::common::packet_log::Config packetLogConfig(const ruvia::Env &env,
                                                        const std::filesystem::path &runtime)
    {
        service::common::packet_log::Config config;
        config.directory = runtime / "logs";
        config.level =
            service::common::packet_log::parseLevel(env.get("PACKET_LOG_LEVEL").value_or("DEBUG"));
        return config;
    }

    bool envFlag(const ruvia::Env &env, std::string_view name, bool fallback = false)
    {
        const auto value = env.get(name);
        if (!value)
            return fallback;
        std::string normalized(*value);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        return normalized == "1" || normalized == "true" || normalized == "yes" ||
               normalized == "on";
    }

    AppConfig gb28181Config(const ruvia::Env &env)
    {
        AppConfig config;
        config.enabled = envFlag(env, "GB28181_ENABLED");
        config.sip.domain = std::string(env.get("GB28181_SIP_DOMAIN").value_or(""));
        config.sip.id = std::string(env.get("GB28181_SIP_ID").value_or(""));
        config.sip.host =
            std::string(env.get("GB28181_SIP_HOST").value_or("0.0.0.0"));
        config.sip.publicIp =
            std::string(env.get("GB28181_SIP_PUBLIC_IP").value_or(""));
        config.sip.port = env.get<std::uint16_t>("GB28181_SIP_PORT").value_or(5060);
        config.sip.password =
            std::string(env.get("GB28181_SIP_PASSWORD").value_or(""));
        config.sip.transport =
            std::string(env.get("GB28181_SIP_TRANSPORT").value_or("udp"));
        config.sip.registrationTimeoutSeconds =
            env.get<int>("GB28181_REGISTRATION_TIMEOUT_SECONDS").value_or(180);
        config.sip.commandTimeoutSeconds =
            env.get<int>("GB28181_COMMAND_TIMEOUT_SECONDS").value_or(10);
        config.sip.inviteTimeoutSeconds =
            env.get<int>("GB28181_INVITE_TIMEOUT_SECONDS").value_or(15);
        config.sip.viewerLeaseTimeoutSeconds =
            env.get<int>("GB28181_VIEWER_LEASE_TIMEOUT_SECONDS").value_or(90);
        config.sip.nonceTtlSeconds =
            env.get<int>("GB28181_NONCE_TTL_SECONDS").value_or(300);
        config.sip.deviceTimezoneOffsetMinutes =
            env.get<int>("GB28181_DEVICE_TIMEZONE_OFFSET_MINUTES").value_or(480);
        config.sip.logging = envFlag(env, "GB28181_SIP_LOGGING", true);
        config.media.zlmPublicBaseUrl =
            std::string(env.get("ZLM_PUBLIC_BASE_URL").value_or(""));
        config.media.rtpPublicIp =
            std::string(env.get("GB28181_RTP_PUBLIC_IP").value_or(""));
        config.media.playTokenSecret =
            std::string(env.get("GB28181_MEDIA_TOKEN_SECRET").value_or(""));
        config.media.playTokenTtlSeconds =
            env.get<int>("GB28181_MEDIA_TOKEN_TTL_SECONDS").value_or(300);
        config.media.corsOrigin =
            std::string(env.get("GB28181_MEDIA_CORS_ORIGIN").value_or(""));
        config.media.workerThreads = env.get<int>("ZLM_WORKER_THREADS").value_or(1);
        config.media.logLevel = env.get<int>("ZLM_LOG_LEVEL").value_or(2);
        config.media.httpPort = env.get<std::uint16_t>("ZLM_HTTP_PORT").value_or(8080);
        config.media.httpsPort = env.get<std::uint16_t>("ZLM_HTTPS_PORT").value_or(8443);
        config.media.rtspPort = env.get<std::uint16_t>("ZLM_RTSP_PORT").value_or(8554);
        config.media.rtspsPort = env.get<std::uint16_t>("ZLM_RTSPS_PORT").value_or(8322);
        config.media.rtmpPort = env.get<std::uint16_t>("ZLM_RTMP_PORT").value_or(1935);
        config.media.rtmpsPort = env.get<std::uint16_t>("ZLM_RTMPS_PORT").value_or(1936);
        config.media.rtcPort = env.get<std::uint16_t>("ZLM_RTC_PORT").value_or(8000);
        config.media.srtPort = env.get<std::uint16_t>("ZLM_SRT_PORT").value_or(9000);
        config.media.rtpPortRangeStart =
            env.get<std::uint16_t>("GB28181_RTP_PORT_START").value_or(30000);
        config.media.rtpPortRangeEnd =
            env.get<std::uint16_t>("GB28181_RTP_PORT_END").value_or(30500);
        config.media.tlsEnabled = envFlag(env, "ZLM_TLS_ENABLED");
        config.media.tlsPemPath =
            std::string(env.get("ZLM_TLS_PEM_PATH").value_or(""));
        config.media.tlsPassword =
            std::string(env.get("ZLM_TLS_PASSWORD").value_or(""));
        config.media.recordingEnabled = envFlag(env, "GB28181_RECORDING_ENABLED");
        config.media.recordRoot =
            std::string(env.get("GB28181_RECORD_ROOT").value_or(""));
        config.media.recordMaxSegmentSeconds =
            env.get<std::uint32_t>("GB28181_RECORD_MAX_SEGMENT_SECONDS").value_or(3600);
        return config;
    }

    void configureWeb(ruvia::App &app, const std::filesystem::path &runtime)
    {
        const auto webRoot = runtime / "web";
        if (!std::filesystem::is_directory(webRoot))
            return;
        ruvia::DocumentRootConfig config;
        config.root = webRoot;
        config.staticOptions.indexFile = "index.html";
        config.staticOptions.cacheControl = "no-cache";
        app.documentRoot(std::move(config));
    }

    ruvia::Task<ruvia::HttpResponse> handleError(ruvia::Context &c, ruvia::HttpErrorInfo info)
    {
        c.status(info.status());
        const auto message = info.message().empty() ? std::string_view("请求失败") : info.message();
        co_return c.json(service::common::error(
            c, service::common::errorCode(info.code(), info.status().value()), message));
    }

    ruvia::Task<void>
    startCollector(ruvia::WebWorkerContext &context,
                     std::shared_ptr<service::collector::Runtime> collector,
                     ruvia::RedisConfig redis, std::size_t workerCount,
                     std::shared_ptr<std::promise<void>> started)
    {
        try
        {
            (void)co_await service::runtime::project(context);
            collector->start(std::move(redis), workerCount);
            // Collector startup clears the shared ephemeral device runtime namespace.
            // Rehydrate telemetry only after that reset so live updates retain the
            // device identity required by the Redis projection script.
            co_await service::telemetry::latest::hydrate(context);
            started->set_value();
        }
        catch (...)
        {
            try
            {
                started->set_exception(std::current_exception());
            }
            catch (...)
            {
            }
        }
    }

} // namespace

int main(int argc, char *argv[])
{
    try
    {
        const bool migrateOnly =
            argc == 2 && std::string_view(argv[1]) == "--migrate-only";
        if (argc > 1 && !migrateOnly)
            throw std::invalid_argument("usage: server [--migrate-only]");

        auto &app = ruvia::app();
        app.loadDotenv();
        if (!service::edge::protocol::configurePublicBaseUrl(
                app.env().get("EDGE_PUBLIC_BASE_URL")
                    .value_or(service::edge::protocol::kDefaultPublicBaseUrl)))
            throw std::runtime_error("EDGE_PUBLIC_BASE_URL is invalid");
        const auto runtime = runtimeDirectory(argc > 0 ? argv[0] : nullptr);
        service::common::packet_log::initialize(packetLogConfig(app.env(), runtime));
        const auto gb28181 = gb28181Config(app.env());
        service::gb28181::runtime().configure(gb28181);

        auto db = databaseConfig(app.env());
        const auto storagePolicy = service::config::deviceDataStoragePolicy(app.env());
        const auto storagePolicyMigration =
            service::config::deviceDataStoragePolicyMigration(storagePolicy);
        std::vector<ruvia::DbMigration> migrations;
        migrations.reserve(service::config::kSchemaMigrations.size() + 1);
        migrations.insert(migrations.end(), service::config::kSchemaMigrations.begin(),
                          service::config::kSchemaMigrations.end());
        migrations.emplace_back(ruvia::DbMigrationOptions{
            .id = storagePolicyMigration.id,
            .sql = storagePolicyMigration.sql,
        });
        ruvia::DbMigratorOptions migrationOptions;
        migrationOptions.table = "sys_schema_migrations";
        const auto report =
            ruvia::DbMigrator::migrate(db, migrations, std::move(migrationOptions));
        std::cout << "database migrations: applied=" << report.applied().size()
                  << ", skipped=" << report.skipped().size() << '\n';
        std::cout << "device_data storage policy: chunk="
                  << storagePolicy.chunkIntervalHours << "h, compression="
                  << (storagePolicy.compressionEnabled
                          ? std::to_string(storagePolicy.compressionAfterHours) + "h"
                          : "disabled")
                  << ", mutable-window=" << storagePolicy.mutableWindowHours << "h\n";
        if (migrateOnly)
            return 0;

        configureWeb(app, runtime);
        const auto cpu = std::max(2U, std::thread::hardware_concurrency());
        const auto mediaWorkers =
            static_cast<unsigned>(std::max(1, gb28181.media.workerThreads));
        // GB28181 owns one SIP actor, one media proxy loop, and two ZLM pools
        // (EventPoller and WorkThread). Reserve those threads before splitting
        // the remaining business budget between northbound/southbound workers.
        const auto gb28181WorkerCount =
            gb28181.enabled ? 2U + 2U * mediaWorkers : 0U;
        // Service and Collector each need at least one worker. On a host with
        // fewer CPUs than that hard minimum plus the enabled media runtime,
        // controlled oversubscription is unavoidable and remains explicit.
        const auto businessCpu =
            std::max(2U, cpu > gb28181WorkerCount
                             ? cpu - gb28181WorkerCount
                             : 0U);
        const auto resolveWorkerCount = [](std::optional<unsigned> configured,
                                           unsigned automatic, const char* name) {
            const auto count = configured.value_or(automatic);
            if (count == 0U || count > 64U)
                throw std::runtime_error(std::string(name) + " must be between 1 and 64");
            return static_cast<std::size_t>(count);
        };
        const auto serviceWorkerCount = resolveWorkerCount(
            app.env().get<unsigned>("SERVICE_WORKERS"), (businessCpu + 1U) / 2U,
            "SERVICE_WORKERS");
        const auto collectorWorkerCount = resolveWorkerCount(
            app.env().get<unsigned>("COLLECTOR_WORKERS"), businessCpu / 2U,
            "COLLECTOR_WORKERS");
        std::cout << "worker budget: cpu=" << cpu
                  << ", service=" << serviceWorkerCount
                  << ", collector=" << collectorWorkerCount
                  << ", gb28181=" << gb28181WorkerCount << '\n';
        auto serviceRedis = redisConfig(app.env());
        auto collectorRedis = serviceRedis;
        // Every Service Worker runs the same seven Stream consumers. Each consumer
        // combines all of its worker-owned shards on one lazy blocking connection.
        serviceRedis.blockingPoolSizePerWorker = 7;
        auto collector = std::make_shared<service::collector::Runtime>();
        auto telemetry = std::make_shared<service::telemetry::PersistenceRuntime>();
        auto commandResults = std::make_shared<service::command::ResultRuntime>();
        auto openWebhooks = std::make_shared<service::access::WebhookRuntime>();
        auto configReconciler = std::make_shared<service::runtime::Reconciler>();
        auto edgeProjector = std::make_shared<service::edge::Projector>();
        auto gb28181Projector = gb28181.enabled
                                    ? std::make_shared<service::gb28181::Projector>()
                                    : nullptr;
        auto alerts = std::make_shared<service::alert::Runtime>();
        auto observability = std::make_shared<service::observability::Registry>();
        service::observability::configureProcessRegistry(*observability);
        observability->gauge("iot_engine_service_workers",
                             static_cast<std::int64_t>(serviceWorkerCount));
        observability->gauge("iot_engine_collector_workers",
                             static_cast<std::int64_t>(collectorWorkerCount));
        service::message::outbox::Policy outboxPolicy;
        outboxPolicy.pendingAlertThreshold =
            app.env().get<std::int64_t>("OUTBOX_PENDING_ALERT_THRESHOLD").value_or(1000);
        outboxPolicy.oldestAgeAlertMs =
            app.env().get<std::int64_t>("OUTBOX_OLDEST_AGE_ALERT_MS").value_or(300000);
        outboxPolicy.deadLetterAlertThreshold =
            app.env().get<std::int64_t>("OUTBOX_DEAD_LETTER_ALERT_THRESHOLD").value_or(1);
        outboxPolicy.receiptRetentionDays =
            app.env().get<std::int64_t>("OUTBOX_RECEIPT_RETENTION_DAYS").value_or(30);
        if (outboxPolicy.pendingAlertThreshold < 0 || outboxPolicy.oldestAgeAlertMs < 0 ||
            outboxPolicy.deadLetterAlertThreshold < 0 ||
            outboxPolicy.receiptRetentionDays < 0 || outboxPolicy.receiptRetentionDays > 3650)
            throw std::runtime_error("OUTBOX policy values are invalid");
        auto outbox = std::make_shared<service::message::outbox::Runtime>(
            *observability, collectorWorkerCount, serviceWorkerCount, outboxPolicy);
        auto applicationRuntime =
            std::make_shared<service::application::Runtime>(*observability);
        app.useWorkerState<service::edge::Dispatcher>()
            .database(ruvia::DbRegistrationConfig{.config = std::move(db)})
            .redis(ruvia::RedisRegistrationConfig{.config = std::move(serviceRedis)})
            .onStart([collector, telemetry, commandResults, openWebhooks, configReconciler,
                      edgeProjector, gb28181Projector, alerts, outbox,
                      applicationRuntime,
                      collectorRedis = std::move(collectorRedis),
                      collectorWorkerCount, &app]() mutable
                     {
                auto workers = app.workers();
                if (workers.empty())
                    throw std::runtime_error(
                        "service: no worker available for config projection");
                applicationRuntime->add({
                    .name = "outbox",
                    .start = [outbox, workers] { outbox->start(workers); },
                    .stop = [outbox] { outbox->stop(); }});
                applicationRuntime->add({
                    .name = "telemetry",
                    .start = [telemetry, workers, collectorWorkerCount] {
                        telemetry->start(workers, collectorWorkerCount);
                    },
                    .stop = [telemetry] { telemetry->stop(); }});
                applicationRuntime->add({
                    .name = "command-results",
                    .start = [commandResults, workers, collectorWorkerCount] {
                        commandResults->start(workers, collectorWorkerCount);
                    },
                    .stop = [commandResults] { commandResults->stop(); }});
                applicationRuntime->add({
                    .name = "webhooks",
                    .dependencies = {"outbox"},
                    .start = [openWebhooks, workers] { openWebhooks->start(workers); },
                    .stop = [openWebhooks] { openWebhooks->stop(); }});
                applicationRuntime->add({
                    .name = "edge-dispatcher",
                    .start = [workers] {
                        service::edge::dispatcherRuntime().start(workers);
                    },
                    .stop = [] { service::edge::dispatcherRuntime().stop(); }});
                applicationRuntime->add({
                    .name = "edge-projector",
                    .start = [edgeProjector, workers] {
                        edgeProjector->start(workers);
                    },
                    .stop = [edgeProjector] { edgeProjector->stop(); }});
                applicationRuntime->add({
                    .name = "alerts",
                    .dependencies = {"telemetry"},
                    .start = [alerts, workers] { alerts->start(workers); },
                    .stop = [alerts] { alerts->stop(); }});
                if (gb28181Projector) {
                    applicationRuntime->add({
                        .name = "gb28181",
                        .start = [gb28181Projector, workers] {
                            auto snapshot = gb28181Projector->start(workers);
                            service::gb28181::runtime().attachProjector(
                                gb28181Projector, std::move(snapshot));
                            service::gb28181::runtime().start();
                        },
                        .stop = [gb28181Projector] {
                            service::gb28181::runtime().stop();
                            gb28181Projector->stop();
                        }});
                }
                applicationRuntime->add({
                    .name = "collector",
                    .dependencies = {"telemetry", "command-results", "edge-projector",
                                     "alerts"},
                    .start = [collector, collectorRedis = std::move(collectorRedis),
                              collectorWorkerCount, worker = workers.front()]() mutable {
                        auto started = std::make_shared<std::promise<void>>();
                        auto ready = started->get_future();
                        const auto posted = worker.post(
                            [collector, collectorRedis = std::move(collectorRedis),
                             collectorWorkerCount, started](
                                ruvia::WebWorkerContext& context) mutable -> ruvia::Task<void> {
                                return startCollector(context, collector,
                                                      std::move(collectorRedis),
                                                      collectorWorkerCount, started);
                            });
                        if (!posted.accepted())
                            throw std::runtime_error(
                                "service rejected runtime projection");
                        ready.get();
                    },
                    .stop = [collector] { collector->stop(); }});
                applicationRuntime->add({
                    .name = "config-reconciler",
                    .dependencies = {"collector", "outbox"},
                    .start = [configReconciler, workers, collectorWorkerCount] {
                        configReconciler->start(workers, collectorWorkerCount);
                    },
                    .stop = [configReconciler] { configReconciler->stop(); }});
                applicationRuntime->start(); })
            .onStop([applicationRuntime] { applicationRuntime->stop(); })
            .onError(&handleError)
            .listen(ruvia::ListenConfig{
                .address = std::string(app.env().get("HOST").value_or("0.0.0.0")),
                .http = app.env().get<std::uint16_t>("PORT").value_or(1102),
            })
            .server(ruvia::ServerConfig{
                .workerCount = serviceWorkerCount,
                .maxStreamBodyBytes = 129U * 1024U * 1024U,
                .maxWebSocketMessageBytes = 16U * 1024U,
            })
            .run();
        service::common::packet_log::shutdown();
        return 0;
    }
    catch (const std::exception &error)
    {
        service::common::packet_log::shutdown();
        std::cerr << "server failed: " << error.what() << '\n';
        return 1;
    }
}
