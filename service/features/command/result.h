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
    static constexpr std::size_t kBatchSize = 256;

    ruvia::Task<void> run(ruvia::WebWorkerContext& context, std::size_t index,
                          std::shared_ptr<std::promise<void>> ready,
                          std::shared_ptr<std::promise<void>> stopped) {
        try {
            const auto redis = context.redis();
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
                          redis, streams, kGroup, consumer, "0", kBatchSize)
                    : co_await message::redis::readGroupManyBlocking(
                          redis, streams, kGroup, consumer,
                          context.stopToken(), kBatchSize);
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
                        co_await projectAndAcknowledgeMany(
                            redis, partition, batch.stream, batch.messages);
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
    static ruvia::Task<void>
    projectAndAcknowledgeMany(const Redis& redis, std::size_t partition,
                              std::string_view sourceStream,
                              const std::vector<message::StreamMessage>& messages) {
        if (messages.empty())
            co_return;
        static constexpr std::string_view invalidScript = R"lua(
local values = {'*'}
for index = 3, #ARGV do values[#values + 1] = ARGV[index] end
redis.call('XADD', KEYS[1], 'MAXLEN', '~', 1000, unpack(values))
redis.call('XACK', KEYS[2], ARGV[1], ARGV[2])
redis.call('XDEL', KEYS[2], ARGV[2])
return 1
)lua";
        static constexpr std::string_view validScript = R"lua(
local hash = {}
for index = 9, #ARGV do hash[#hash + 1] = ARGV[index] end
redis.call('HSET', KEYS[1], unpack(hash))
redis.call('PEXPIRE', KEYS[1], ARGV[3])
redis.call('XADD', KEYS[2], 'MAXLEN', '~', 100000, '*',
  'event_id', ARGV[4], 'event_type', 'device.command.responded',
  'device_id', ARGV[5], 'device_code', ARGV[6],
  'occurred_at_ms', ARGV[7], 'data_json', ARGV[8])
redis.call('XACK', KEYS[3], ARGV[1], ARGV[2])
redis.call('XDEL', KEYS[3], ARGV[2])
return 1
)lua";
        auto pipeline = redis.pipeline();
        const auto ttl = std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(kStateTtl).count());
        for (const auto& message : messages) {
            const auto commandId = message.get("command_id");
            if (commandId.empty()) {
                auto fields = message.fields;
                fields.push_back({"source_entry_id", message.id});
                fields.push_back({"failure_reason", "command_result_invalid"});
                fields.push_back({"failed_at_ms", std::to_string(message::utcNowMilliseconds())});
                const auto deadLetter = message::deadLetterStream(partition);
                const std::string source(sourceStream);
                const std::string_view keys[]{deadLetter, source};
                std::vector<std::string_view> arguments{kGroup, message.id};
                arguments.reserve(arguments.size() + fields.size() * 2);
                for (const auto& field : fields) {
                    arguments.push_back(field.name);
                    arguments.push_back(field.value);
                }
                message::redis::queueEval(pipeline, invalidScript, keys, arguments);
                continue;
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
            fields.push_back(
                {"status", message.get("success") == "1" ? "SUCCESS" : "FAILED"});
            const auto key = "iot:state:command:" + std::string(commandId);
            const auto completedAt = std::to_string(message::utcNowMilliseconds());
            const std::string data =
                "{\"commandId\":" + service::access::jsonQuoted(commandId) +
                ",\"status\":" +
                service::access::jsonQuoted(message.get("success") == "1" ? "SUCCESS"
                                                                          : "FAILED") +
                ",\"reason\":" + service::access::jsonQuoted(message.get("reason")) + "}";
            const std::string source(sourceStream);
            const std::string eventStream(service::access::event::kStream);
            const std::string_view keys[]{key, eventStream, source};
            std::vector<std::string_view> arguments{
                kGroup, message.id, ttl, message.get("message_id"),
                message.get("device_id"), message.get("device_code"), completedAt, data};
            arguments.reserve(arguments.size() + fields.size() * 2);
            for (const auto& field : fields) {
                arguments.push_back(field.name);
                arguments.push_back(field.value);
            }
            message::redis::queueEval(pipeline, validScript, keys, arguments);
        }
        const auto replies = co_await std::move(pipeline).exec();
        message::redis::requirePipelineSuccess(
            "project and acknowledge command result batch", replies);
        for (const auto& reply : replies)
            if (reply.kind() != ruvia::RedisValue::Kind::kInteger)
                message::redis::throwValue(
                    "project and acknowledge command result batch", reply);
    }

    std::vector<ruvia::WebWorkerHandle> workers_;
    std::vector<std::shared_future<void>> stopped_;
    std::size_t collectorWorkerCount_ = 0;
    std::atomic_bool running_{false};
};

} // namespace service::command
