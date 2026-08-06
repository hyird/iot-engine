#pragma once

#include <atomic>
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
#include "service/features/access/contract.h"
#include "service/features/access/event.h"
#include "service/features/collector/stream.h"

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
        for (const auto& stopped : stopped_)
            if (stopped.valid())
                (void)stopped.wait_for(std::chrono::seconds(3));
        stopped_.clear();
        workers_.clear();
    }

  private:
    static constexpr std::string_view kGroup = "iot-engine:command-result";
    static constexpr auto kStateTtl = std::chrono::hours(24);

    ruvia::Task<void> run(ruvia::WebWorkerContext& context, std::size_t index,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        try {
            const auto redis = context.redis();
            const auto readRedis = context.redis(message::redis::kBlockingRedisAlias);
            std::vector<std::string> streams;
            std::map<std::string, std::size_t, std::less<>> streamPartitions;
            for (auto partition = index; partition < collectorWorkerCount_;
                  partition += workers_.size()) {
                streams.push_back(message::commandResultStream(partition));
                streamPartitions.emplace(streams.back(), partition);
                co_await message::redis::ensureGroup(
                    redis, streams.back(), kGroup);
            }
            bool recovering = true;
            ready->set_value();
            const auto consumer = "service-" + std::to_string(index);
            while (running_.load() && !context.stopToken().stopRequested()) {
                if (streams.empty()) {
                    (void)co_await ruvia::sleepFor(context.worker(), std::chrono::seconds(1));
                    continue;
                }
                const auto batches = recovering
                    ? co_await message::redis::readGroupMany(
                          redis, streams, kGroup, consumer, "0", 100)
                    : co_await message::redis::readGroupManyBlocking(
                          readRedis, streams, kGroup, consumer,
                          context.stopToken(), 100);
                if (recovering && batches.empty()) {
                    recovering = false;
                    continue;
                }
                bool failed = false;
                for (const auto& batch : batches) {
                    const auto partitionEntry = streamPartitions.find(batch.stream);
                    if (partitionEntry == streamPartitions.end())
                        continue;
                    const auto partition = partitionEntry->second;
                    try {
                        for (const auto& message : batch.messages) {
                            co_await project(redis, partition, message);
                            co_await message::redis::acknowledgeAndDelete(redis, batch.stream,
                                                                               kGroup, message.id);
                        }
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

    template <typename Redis>
    static ruvia::Task<void> project(const Redis& redis, std::size_t partition,
                                     const message::StreamMessage& message) {
        const auto commandId = message.get("command_id");
        if (commandId.empty()) {
            auto fields = message.fields;
            fields.push_back({"source_entry_id", message.id});
            fields.push_back({"failure_reason", "command_result_invalid"});
            fields.push_back({"failed_at_ms", std::to_string(message::utcNowMilliseconds())});
            (void)co_await message::redis::publish(redis, message::deadLetterStream(partition),
                                                        fields, 1000);
            co_return;
        }
        std::vector<message::StreamField> fields;
        fields.reserve(message.fields.size() + 2);
        for (const auto& field : message.fields) {
            if (field.name == "message_id" || field.name == "causation_id" ||
                field.name == "command_id")
                continue;
            fields.push_back(field);
        }
        fields.push_back({"command_id", std::string(commandId)});
        fields.push_back({"status", message.get("success") == "1" ? "SUCCESS" : "FAILED"});
        const auto key = "iot:state:command:" + std::string(commandId);
        co_await message::redis::setHash(redis, key, fields);
        (void)co_await message::redis::command(
            redis, {"PEXPIRE", key,
                    std::to_string(
                        std::chrono::duration_cast<std::chrono::milliseconds>(kStateTtl).count())});
        std::string data =
            "{\"commandId\":" + service::access::jsonQuoted(commandId) + ",\"status\":" +
            service::access::jsonQuoted(message.get("success") == "1" ? "SUCCESS" : "FAILED") +
            ",\"reason\":" + service::access::jsonQuoted(message.get("reason")) + "}";
        co_await service::access::event::publish(
            redis, message.get("message_id"), "device.command.responded", message.get("device_id"),
            message.get("device_code"), message::utcNowMilliseconds(), data);
    }

    std::vector<ruvia::WebWorkerHandle> workers_;
    std::vector<std::shared_future<void>> stopped_;
    std::size_t collectorWorkerCount_ = 0;
    std::atomic_bool running_{false};
};

} // namespace service::command
