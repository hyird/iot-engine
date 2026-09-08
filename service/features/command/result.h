#pragma once

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ruvia/core/Task.h>
#include <ruvia/core/Timer.h>
#include <ruvia/web/WebWorker.h>

#include "service/common/message/contract.h"
#include "service/features/command/repository.h"
#include "service/features/access/contract.h"
#include "service/features/access/event.h"
#include "service/features/collector/stream.h"
#include "service/features/event/stream-multiplexer.h"

namespace service::command {

class ResultRuntime final {
  public:
    ResultRuntime() = default;
    ResultRuntime(const ResultRuntime&) = delete;
    ResultRuntime& operator=(const ResultRuntime&) = delete;
    ~ResultRuntime() { stop(); }

    void start(std::vector<ruvia::WebWorkerHandle> workers, std::size_t collectorWorkerCount) {
        if (running_.exchange(true))
            return;
        workers_ = std::move(workers);
        collectorWorkerCount_ = collectorWorkerCount;
        if (workers_.empty() || collectorWorkerCount_ == 0) {
            running_.store(false);
            throw std::runtime_error("command result runtime requires north and collector workers");
        }
        std::vector<std::future<void>> readiness;
        for (std::size_t index = 0; index < workers_.size(); ++index) {
            auto ready = std::make_shared<std::promise<void>>();
            auto stopped = std::make_shared<std::promise<void>>();
            readiness.push_back(ready->get_future());
            stopped_.push_back(stopped->get_future().share());
            const auto posted = workers_[index].post(
                [this, index, ready, stopped](ruvia::WebWorkerContext& context) {
                    return run(context, index, ready, stopped);
                });
            if (!posted.accepted()) {
                running_.store(false);
                throw std::runtime_error("service worker rejected command result consumer");
            }
        }
        for (auto& ready : readiness)
            ready.get();
    }

    void stop() noexcept {
        if (!running_.exchange(false))
            return;
        service::message::workerStreamMultiplexer().signal(
            service::message::WorkerStreamTask::CommandResult);
        for (const auto& stopped : stopped_)
            if (stopped.valid())
                (void)stopped.wait_for(std::chrono::seconds(3));
        stopped_.clear();
        workers_.clear();
    }

  private:
    static constexpr std::string_view kGroup = "iot-engine:command-result";
    static constexpr auto kStateTtl = std::chrono::hours(24);
    static constexpr std::size_t kBatchSize = 256;

    static std::size_t actualValueCount(const message::StreamMessage& message) {
        const auto value = message.get("actual_value_count");
        std::size_t count{};
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), count);
        if (error != std::errc{} || end != value.data() + value.size())
            return 0;
        return std::min<std::size_t>(count, 8);
    }

    static std::string actualValueField(std::size_t index, std::string_view name) {
        return "actual_value_" + std::to_string(index) + "_" + std::string(name);
    }

    static std::string actualValuesJson(const message::StreamMessage& message) {
        std::string output{"["};
        for (std::size_t index = 0; index < actualValueCount(message); ++index) {
            if (index != 0)
                output.push_back(',');
            output += "{\"elementId\":" + service::access::jsonQuoted(
                          message.get(actualValueField(index, "element_id"))) +
                      ",\"name\":" + service::access::jsonQuoted(
                          message.get(actualValueField(index, "name"))) +
                      ",\"kind\":" + service::access::jsonQuoted(
                          message.get(actualValueField(index, "kind"))) +
                      ",\"value\":" + service::access::jsonQuoted(
                          message.get(actualValueField(index, "value"))) +
                      ",\"unit\":" + service::access::jsonQuoted(
                          message.get(actualValueField(index, "unit"))) + "}";
        }
        output.push_back(']');
        return output;
    }

    ruvia::Task<void> run(ruvia::WebWorkerContext& context, std::size_t index,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        try {
            const auto redis = context.redis();
            std::vector<std::string> streams;
            std::map<std::string, std::size_t, std::less<>> streamPartitions;
            for (auto partition = index; partition < message::shard::kCount;
                  partition += workers_.size()) {
                streams.push_back(message::commandResultStream(partition));
                streamPartitions.emplace(streams.back(), partition);
                co_await message::redis::ensureGroup(
                    redis, streams.back(), kGroup);
            }
            bool recovering = true;
            ready->set_value();
            const auto consumer = service::runtime::instanceId() + ":service-" + std::to_string(index);
            while (running_.load() && !context.stopToken().stopRequested()) {
                try { co_await repository::dispatch(context); }
                catch (const std::exception& error) {
                    std::cerr << "command dispatch failed: " << error.what() << '\n';
                }
                if (streams.empty()) {
                    (void)co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(1));
                    continue;
                }
                std::vector<message::redis::StreamBatch> batches;
                bool readFailed = false;
                try {
                    batches = recovering
                        ? co_await message::redis::claimGroupMany(
                              redis, streams, kGroup, consumer, kBatchSize)
                        : co_await message::redis::readGroupMany(
                              redis, streams, kGroup, consumer, ">", kBatchSize);
                } catch (const std::exception& error) {
                    if (context.stopToken().stopRequested())
                        break;
                    std::cerr << "command result stream read failed for service worker " << index
                              << ": " << error.what() << '\n';
                    recovering = true;
                    readFailed = true;
                }
                if (readFailed) {
                    (void)co_await ruvia::sleepFor(context.worker(),
                                                   std::chrono::milliseconds(250));
                    continue;
                }
                if (recovering && batches.empty()) {
                    recovering = false;
                    continue;
                }
                if (batches.empty()) {
                    co_await service::message::workerStreamMultiplexer().wait(
                        index, service::message::WorkerStreamTask::CommandResult,
                        context.stopToken(), std::chrono::milliseconds(250));
                    continue;
                }
                bool failed = false;
                for (const auto& batch : batches) {
                    const auto partitionEntry = streamPartitions.find(batch.stream);
                    if (partitionEntry == streamPartitions.end())
                        continue;
                    const auto partition = partitionEntry->second;
                    try {
                        co_await projectAndAcknowledgeMany(
                            context, partition, batch.stream, batch.messages);
                    } catch (const std::exception& error) {
                        std::cerr << "command result projection failed for collector worker "
                                  << partition << ": " << error.what() << '\n';
                        recovering = true;
                        failed = true;
                    }
                }
                if (failed)
                    (void)co_await ruvia::sleepFor(context.worker(),
                                                   std::chrono::milliseconds(250));
            }
        } catch (...) {
            try {
                ready->set_exception(std::current_exception());
            } catch (...) {
            }
        }
        try {
            stopped->set_value();
        } catch (...) {
        }
    }

    template <typename Context>
    static ruvia::Task<void>
    projectAndAcknowledgeMany(Context& context, std::size_t partition,
                              std::string_view sourceStream,
                              const std::vector<message::StreamMessage>& messages) {
        (void)partition;
        if (messages.empty()) co_return;
        auto transaction = co_await context.db().beginTransaction();
        for (const auto& message : messages) {
            const auto id = message.get("command_id");
            const auto deviceId = message.get("device_id");
            if (!common::isUuid(id) || !common::isUuid(deviceId)) continue;
            const auto explicitState = message.get("result_state");
            const auto state = terminalState(explicitState) ? explicitState :
                collectorResultState(message.get("success") == "1",message.get("reason"));
            const auto actual = actualValuesJson(message);
            const auto updated = co_await transaction.query(R"sql(
UPDATE command_operation SET status=$3,reason=$4,actual_values=$5::jsonb,completed_at=NOW()
WHERE id=$1::uuid AND device_id=$2::uuid
 AND (status IN ('DISPATCHING','AWAITING_RESULT') OR (status='UNKNOWN' AND $3<>'UNKNOWN'))
RETURNING id::text)sql", common::dbParams(id,deviceId,state,message.get("reason"),actual));
            if (!updated.empty())
                co_await repository::event(transaction,id,"device.command.updated");
        }
        co_await transaction.commit();
        co_await message::redis::acknowledgeAndDeleteMany(context.redis(),sourceStream,kGroup,messages);
    }

    std::vector<ruvia::WebWorkerHandle> workers_;
    std::vector<std::shared_future<void>> stopped_;
    std::size_t collectorWorkerCount_ = 0;
    std::atomic_bool running_{false};
};

} // namespace service::command
