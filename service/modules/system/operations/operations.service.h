#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/web/Context.h>
#include <ruvia/web/db/Db.h>
#include <ruvia/web/redis/RedisRepository.h>
#include "service/modules/system/operations/operations.entity.h"

#include "service/common/message.h"
#include "service/common/observability.h"

namespace service::system {

class OperationsService final {
  public:
    struct Readiness {
        bool ready;
        std::string json;
    };

    static ruvia::Task<Readiness> readiness(ruvia::Context& context) {
        const auto expectedWorkers = workerCount();
        bool databaseReady = false;
        bool redisReady = false;
        try {
            ruvia::DbQuery query(context.pool());
            query.select(query.value(1));
            databaseReady = !(co_await context.db().query(query)).empty();
            const std::vector<std::string_view> command{ "PING" };
            const auto reply = co_await context.redis().command(command);
            redisReady = reply.kind() == ruvia::RedisValue::Kind::kString &&
                reply.string() == "PONG";
        } catch (...) {
        }

        std::vector<WorkerSnapshot> snapshots;
        bool snapshotsAvailable = expectedWorkers != 0;
        if (snapshotsAvailable) {
            try {
                snapshots = co_await loadSnapshots(context, expectedWorkers);
            } catch (...) {
                snapshotsAvailable = false;
            }
        }

        bool workersReady = snapshotsAvailable && snapshots.size() == expectedWorkers;
        bool degraded = false;
        for (const auto& snapshot : snapshots) {
            if (!snapshot.metricsPresent || !snapshot.readinessPresent || !snapshot.ready ||
                snapshot.healthJson.empty()) {
                workersReady = false;
            }
            if (snapshot.healthJson.find("\"status\":\"degraded\"") != std::string::npos) {
                degraded = true;
            }
        }

        const bool ready = databaseReady && redisReady && workersReady;
        const auto status = !ready ? std::string_view{ "not_ready" }
            : degraded             ? std::string_view{ "degraded" }
                                   : std::string_view{ "ready" };
        co_return Readiness{ ready, aggregateHealthJson(snapshots, status) };
    }

    static ruvia::Task<std::string> metrics(ruvia::Context& context) {
        const auto expectedWorkers = workerCount();
        if (expectedWorkers == 0) {
            co_return fallbackMetrics();
        }
        try {
            const auto snapshots = co_await loadSnapshots(context, expectedWorkers);
            std::set<std::string, std::less<>> typeLines;
            std::string output;
            for (const auto& snapshot : snapshots) {
                if (!snapshot.metricsPresent) {
                    continue;
                }
                std::istringstream lines(snapshot.metrics);
                for (std::string line; std::getline(lines, line);) {
                    if (line.starts_with("# TYPE ") && !typeLines.emplace(line).second) {
                        continue;
                    }
                    output.append(line);
                    output.push_back('\n');
                }
            }
            co_return output.empty() ? fallbackMetrics() : std::move(output);
        } catch (...) {
            co_return fallbackMetrics();
        }
    }

  private:
    struct WorkerSnapshot final {
        bool metricsPresent{ false };
        bool readinessPresent{ false };
        bool ready{ false };
        std::string metrics;
        std::string healthJson;
    };

    static std::size_t workerCount() noexcept {
        const auto* diagnostics = observability::currentWorkerDiagnostics();
        return diagnostics ? diagnostics->workerCount() : 0;
    }

    static ruvia::Task<std::vector<WorkerSnapshot>>
    loadSnapshots(ruvia::Context& context, std::size_t count) {
        auto snapshots = context.redis().getRepository<WorkerSnapshotEntity>();
        std::vector<WorkerSnapshot> result(count);
        for (std::size_t index = 0; index < count; ++index) {
            const ruvia::DbFindOptions options{
                .where = WorkerSnapshotEntity::column<"id">() ==
                    service::message::worker_metrics::snapshotId(index)};
            const auto snapshot = co_await snapshots.findOne(options);
            if (!snapshot) continue;
            auto& value = result[index];
            value.metrics.assign(snapshot->get<"metrics">().view());
            value.healthJson.assign(snapshot->get<"health">().view());
            value.metricsPresent = true;
            value.readinessPresent = true;
            value.ready = snapshot->get<"ready">();
        }
        co_return result;
    }

    static std::string aggregateHealthJson(const std::vector<WorkerSnapshot>& snapshots, std::string_view status) {
        std::string result;
        for (const auto& snapshot : snapshots) {
            if (!snapshot.healthJson.empty()) {
                result = snapshot.healthJson;
                break;
            }
        }
        if (result.empty()) {
            result = "{\"status\":\"not_ready\",\"components\":{},\"alerts\":{}}";
        }

        constexpr std::string_view prefix{ "\"status\":\"" };
        if (const auto start = result.find(prefix); start != std::string::npos) {
            const auto valueStart = start + prefix.size();
            if (const auto valueEnd = result.find('"', valueStart);
                valueEnd != std::string::npos) {
                result.replace(valueStart, valueEnd - valueStart, status);
            }
        }

        if (result.back() == '}') {
            result.pop_back();
        }
        result += ",\"workers\":{";
        for (std::size_t index = 0; index < snapshots.size(); ++index) {
            if (index != 0) {
                result.push_back(',');
            }
            result += '"' + std::to_string(index) + "\":";
            if (snapshots[index].readinessPresent && !snapshots[index].healthJson.empty()) {
                result += snapshots[index].healthJson;
            } else {
                result += "{\"status\":\"not_ready\",\"components\":{}}";
            }
        }
        result += "}}";
        return result;
    }

    static std::string fallbackMetrics() {
        return "# TYPE iot_engine_ready gauge\n"
               "iot_engine_ready 0\n";
    }
};

} // namespace service::system
