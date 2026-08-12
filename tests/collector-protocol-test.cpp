#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <memory_resource>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio.hpp>
#include <ruvia/core/detail/io/AsioAwait.h>
#include <ruvia/web/detail/redis/RedisTypesAccess.h>

#include "service/common/packet-log.h"
#include "service/features/alert/metadata.h"
#include "service/features/collector/command.h"
#include "service/features/collector/modbus.h"
#include "service/features/collector/engine.h"
#include "service/features/collector/poll.h"
#include "service/features/collector/reconcile.h"
#include "service/features/collector/s7.h"
#include "service/features/collector/sl651.h"
#include "service/features/collector/stream.h"
#include "service/features/collector/config.h"
#include "service/features/collector/tcp.h"
#include "service/features/collector/timer.h"
#include "service/features/command/queue.h"
#include "service/features/edge/session.h"
#include "service/features/runtime/repository.h"
#include "service/features/telemetry/latest.h"

namespace collector = service::collector;

namespace {

template <typename Result> Result runTask(ruvia::Task<Result> task) {
    asio::io_context context;
    std::optional<Result> result;
    std::exception_ptr exception;
    asio::co_spawn(
        context,
        [task = std::move(task), &result, &exception]() mutable -> asio::awaitable<void> {
            try {
                result.emplace(co_await ruvia::detail::taskAsAwaitable(std::move(task)));
            } catch (...) {
                exception = std::current_exception();
            }
        },
        asio::detached);
    context.run();
    if (exception)
        std::rethrow_exception(exception);
    if (!result)
        throw std::runtime_error("task produced no result");
    return std::move(*result);
}

void runTask(ruvia::Task<void> task) {
    asio::io_context context;
    bool completed = false;
    std::exception_ptr exception;
    asio::co_spawn(
        context,
        [task = std::move(task), &completed, &exception]() mutable -> asio::awaitable<void> {
            try {
                co_await ruvia::detail::taskAsAwaitable(std::move(task));
                completed = true;
            } catch (...) {
                exception = std::current_exception();
            }
        },
        asio::detached);
    context.run();
    if (exception)
        std::rethrow_exception(exception);
    if (!completed)
        throw std::runtime_error("task produced no completion");
}

struct FakeDbCell {
    std::string value;
    bool null = false;

    [[nodiscard]] std::string_view text() const noexcept { return value; }
    [[nodiscard]] bool isNull() const noexcept { return null; }
};

struct FakeDbRow {
    std::vector<FakeDbCell> cells;

    explicit FakeDbRow(std::initializer_list<std::string_view> values) {
        cells.reserve(values.size());
        for (const auto value : values)
            cells.push_back({std::string(value), false});
    }

    [[nodiscard]] const FakeDbCell& operator[](std::size_t index) const {
        return cells.at(index);
    }
};

struct FakeDbResult {
    std::vector<FakeDbRow> values;

    [[nodiscard]] const std::vector<FakeDbRow>& rows() const noexcept {
        return values;
    }
};

struct RuntimeRepositoryScaleDb {
    ruvia::Task<FakeDbResult> query(std::string_view sql) {
        FakeDbResult result;
        if (sql.find("FROM link\r\nWHERE deleted_at IS NULL AND execution = 'collector'") !=
            std::string_view::npos) {
            result.values.emplace_back(FakeDbRow{
                "link-1", "collector link", "TCP Client", "Modbus", "127.0.0.1", "1502",
                "enabled"});
        } else if (sql.find("ORDER BY d.link_id, d.id") != std::string_view::npos) {
            result.values.emplace_back(FakeDbRow{
                "device-1", "MODBUS001", "modbus device", "link-1", "TCP Client", "",
                "Modbus", "+08:00", "300", "OFF", "", "OFF", "", "TCP", "1",
                "RACK_SLOT", "PG", "0", "1", "0100", "0101", "5000", "5000",
                "STANDARD", "5", "1", "60", "1", "100", "125"});
        } else if (sql.find("p.protocol = 'Modbus'") != std::string_view::npos &&
                   sql.find("ORDER BY d.id,") != std::string_view::npos) {
            result.values.emplace_back(FakeDbRow{
                "device-1", "temperature", "Temperature", "℃", "UINT16", "BIG_ENDIAN",
                "HOLDING_REGISTER", "0", "1", "1x", "-1", "f"});
        }
        co_return result;
    }
};

struct RecordingRedis {
    explicit RecordingRedis(std::int64_t result)
        : reply(ruvia::detail::RedisTypesAccess::integerValue(
              result, std::pmr::get_default_resource())) {}

    ruvia::Task<ruvia::RedisValue>
    eval(std::string_view value, std::span<const std::string_view> inputKeys,
         std::span<const std::string_view> inputArguments) const {
        script.assign(value);
        keys.assign(inputKeys.begin(), inputKeys.end());
        arguments.assign(inputArguments.begin(), inputArguments.end());
        co_return reply;
    }

    mutable std::string script;
    mutable std::vector<std::string> keys;
    mutable std::vector<std::string> arguments;
    ruvia::RedisValue reply;
};

struct EdgeSessionRedis {
    ruvia::Task<void> setEx(std::string_view, std::chrono::seconds,
                            std::string_view epoch) const {
        value = std::string(epoch);
        co_return;
    }

    ruvia::Task<std::int64_t> del(std::string_view) const {
        const auto removed = value.has_value();
        value.reset();
        co_return removed ? 1 : 0;
    }

    ruvia::Task<ruvia::RedisValue>
    eval(std::string_view script, std::span<const std::string_view>,
         std::span<const std::string_view> arguments) const {
        if (arguments.empty() || !value || *value != arguments.front())
            co_return ruvia::detail::RedisTypesAccess::integerValue(
                0, std::pmr::get_default_resource());
        if (script.find("redis.call('DEL'") != std::string_view::npos)
            value.reset();
        co_return ruvia::detail::RedisTypesAccess::integerValue(
            1, std::pmr::get_default_resource());
    }

    mutable std::optional<std::string> value;
};

struct AlertScheduleRedisState {
    std::string script;
    std::vector<std::vector<std::string>> pipelineCommands;
    bool invalidStoredDuration = true;
};

struct AlertScheduleRedis {
    std::shared_ptr<AlertScheduleRedisState> state =
        std::make_shared<AlertScheduleRedisState>();

    struct Pipeline {
        std::shared_ptr<AlertScheduleRedisState> state;
        std::vector<std::vector<std::string>> commands;

        void command(std::span<const std::string_view> arguments) {
            commands.emplace_back(arguments.begin(), arguments.end());
        }

        ruvia::Task<std::pmr::vector<ruvia::RedisValue>> exec() && {
            state->pipelineCommands.insert(state->pipelineCommands.end(), commands.begin(),
                                           commands.end());
            std::pmr::vector<ruvia::RedisValue> replies;
            replies.reserve(commands.size());
            for (const auto& command : commands) {
                if (!command.empty() && command.front() == "EVALSHA" &&
                    state->invalidStoredDuration &&
                    state->script.find("if duration then") == std::string::npos) {
                    replies.push_back(ruvia::detail::RedisTypesAccess::errorValue(
                        "attempt to perform arithmetic on a nil value",
                        std::pmr::get_default_resource()));
                    continue;
                }
                replies.push_back(ruvia::detail::RedisTypesAccess::integerValue(
                    1, std::pmr::get_default_resource()));
            }
            co_return replies;
        }
    };

    ruvia::Task<std::string> scriptLoad(std::string_view script) const {
        state->script = std::string(script);
        co_return "alert-schedule-sha";
    }

    [[nodiscard]] Pipeline pipeline() const { return Pipeline{state}; }
};

struct GroupedBoundedRedis {
    mutable std::string script;
    bool invalidDepth = true;

    ruvia::Task<ruvia::RedisValue>
    eval(std::string_view value, std::span<const std::string_view>,
         std::span<const std::string_view>) const {
        script = std::string(value);
        if (invalidDepth &&
            script.find("local depth = tonumber(redis.call('GET', KEYS[2]) or '0')\n"
                        "if depth >=") != std::string::npos) {
            co_return ruvia::detail::RedisTypesAccess::errorValue(
                "attempt to compare nil with number",
                std::pmr::get_default_resource());
        }
        co_return ruvia::detail::RedisTypesAccess::stringValue(
            "1-0", std::pmr::get_default_resource());
    }
};

struct GroupedAckRedis {
    mutable std::string script;
    mutable int depth = 5;

    ruvia::Task<ruvia::RedisValue>
    eval(std::string_view value, std::span<const std::string_view>,
         std::span<const std::string_view>) const {
        script = std::string(value);
        const bool removed = false;
        if (script.find("if removed > 0 then") == std::string::npos || removed) {
            --depth;
        }
        co_return ruvia::detail::RedisTypesAccess::integerValue(
            0, std::pmr::get_default_resource());
    }
};

struct FailingLatestPipeline {
    ruvia::Task<std::pmr::vector<ruvia::RedisValue>> exec() && {
        std::pmr::vector<ruvia::RedisValue> replies;
        replies.push_back(ruvia::detail::RedisTypesAccess::errorValue(
            "injected latest projection failure", std::pmr::get_default_resource()));
        co_return replies;
    }
};

struct FailingConfigRedis {
    explicit FailingConfigRedis(bool failSet = true, bool activeVersion = false)
        : failActiveSet(failSet), hasActiveVersion(activeVersion) {}

    struct Pipeline {
        const FailingConfigRedis* owner;
        std::vector<std::vector<std::string>> commands;

        void command(std::span<const std::string_view> arguments) {
            commands.emplace_back(arguments.begin(), arguments.end());
        }

        ruvia::Task<std::pmr::vector<ruvia::RedisValue>> exec() && {
            owner->pipelineCommands.insert(owner->pipelineCommands.end(), commands.begin(),
                                           commands.end());
            std::pmr::vector<ruvia::RedisValue> replies;
            replies.reserve(commands.size());
            for (std::size_t index = 0; index < commands.size(); ++index)
                replies.push_back(ruvia::detail::RedisTypesAccess::integerValue(
                    1, std::pmr::get_default_resource()));
            co_return replies;
        }
    };

    [[nodiscard]] Pipeline pipeline() const { return Pipeline{this}; }

    ruvia::Task<ruvia::RedisValue>
    command(std::span<const std::string_view> arguments) const {
        if (arguments.empty())
            co_return ruvia::detail::RedisTypesAccess::errorValue(
                "empty command", std::pmr::get_default_resource());
        commands.emplace_back(arguments.begin(), arguments.end());
        if (arguments.front() == "GET" && !hasActiveVersion)
            co_return ruvia::detail::RedisTypesAccess::nullValue(
                std::pmr::get_default_resource());
        if (arguments.front() == "GET")
            co_return ruvia::detail::RedisTypesAccess::stringValue(
                "previous-version", std::pmr::get_default_resource());
        if (arguments.front() == "HGET")
            co_return ruvia::detail::RedisTypesAccess::stringValue(
                "different-signature", std::pmr::get_default_resource());
        if (arguments.front() == "SET" && failActiveSet)
            co_return ruvia::detail::RedisTypesAccess::errorValue(
                "injected active pointer failure", std::pmr::get_default_resource());
        if (arguments.front() == "SET")
            co_return ruvia::detail::RedisTypesAccess::stringValue(
                "OK", std::pmr::get_default_resource());
        if (arguments.front() == "ZRANGEBYSCORE") {
            std::pmr::vector<ruvia::RedisValue> values;
            co_return ruvia::detail::RedisTypesAccess::arrayValue(
                std::move(values), std::pmr::get_default_resource());
        }
        co_return ruvia::detail::RedisTypesAccess::integerValue(
            1, std::pmr::get_default_resource());
    }

    mutable std::vector<std::vector<std::string>> commands;
    mutable std::vector<std::vector<std::string>> pipelineCommands;
    bool failActiveSet;
    bool hasActiveVersion;
};

struct RecordingLatestRedisState {
    std::vector<std::vector<std::string>> pipelineCommands;
    std::vector<std::vector<std::string>> directCommands;
    std::string preservedOnlineUntilMs{"2000000000000"};
};

struct RecordingLatestRedis {
    std::shared_ptr<RecordingLatestRedisState> state =
        std::make_shared<RecordingLatestRedisState>();

    struct Pipeline {
        std::shared_ptr<RecordingLatestRedisState> state;
        std::vector<std::vector<std::string>> commands;

        void command(std::span<const std::string_view> arguments) {
            commands.emplace_back(arguments.begin(), arguments.end());
        }

        static ruvia::RedisValue string(std::string_view value) {
            return ruvia::detail::RedisTypesAccess::stringValue(
                value, std::pmr::get_default_resource());
        }

        static ruvia::RedisValue existingLatestReply() {
            std::pmr::vector<ruvia::RedisValue> values;
            values.push_back(string("device-1"));
            values.push_back(string("{\"temperature\":true}"));
            return ruvia::detail::RedisTypesAccess::arrayValue(
                std::move(values), std::pmr::get_default_resource());
        }

        ruvia::RedisValue existingRuntimeReply() const {
            std::pmr::vector<ruvia::RedisValue> values;
            values.push_back(string("device-1"));
            values.push_back(string(state->preservedOnlineUntilMs));
            return ruvia::detail::RedisTypesAccess::arrayValue(
                std::move(values), std::pmr::get_default_resource());
        }

        ruvia::Task<std::pmr::vector<ruvia::RedisValue>> exec() && {
            state->pipelineCommands.insert(state->pipelineCommands.end(), commands.begin(),
                                           commands.end());
            std::pmr::vector<ruvia::RedisValue> replies;
            replies.reserve(commands.size());
            for (const auto& command : commands) {
                if (command.size() >= 3 && command[0] == "HMGET" &&
                    command[1] == "iot:device:D1:latest")
                    replies.push_back(existingLatestReply());
                else if (command.size() >= 3 && command[0] == "HMGET" &&
                         command[1] == "iot:runtime:device:D1")
                    replies.push_back(existingRuntimeReply());
                else
                    replies.push_back(ruvia::detail::RedisTypesAccess::integerValue(
                        1, std::pmr::get_default_resource()));
            }
            co_return replies;
        }
    };

    [[nodiscard]] Pipeline pipeline() const { return Pipeline{state}; }

    ruvia::Task<ruvia::RedisValue>
    command(std::span<const std::string_view> arguments) const {
        state->directCommands.emplace_back(arguments.begin(), arguments.end());
        co_return ruvia::detail::RedisTypesAccess::integerValue(
            1, std::pmr::get_default_resource());
    }
};

struct LatestProjectionDb {
    template <typename Sql, typename Params> ruvia::Task<FakeDbResult> query(const Sql& sql,
                                                                             const Params&) {
        const std::string text(sql);
        FakeDbResult result;
        if (text.find("CASE WHEN p.protocol = 'SL651'") != std::string::npos) {
            result.values.emplace_back(FakeDbRow{"device-1", "D1", "300000"});
        } else if (text.find("WITH configured AS") != std::string::npos) {
            result.values.emplace_back(FakeDbRow{
                "device-1",
                "D1",
                "Modbus",
                "temperature",
                "New Name",
                "℃",
                "12.5",
                "1700000000000",
                "2",
                "1",
                "environment",
                "",
                "0",
                "{\"id\":\"temperature\",\"name\":\"New Name\",\"value\":\"12.5\",\"unit\":\"℃\","
                "\"scale\":2,\"decimals\":1,\"group\":\"environment\",\"encode\":\"\","
                "\"sort\":0,\"protocol\":\"Modbus\",\"observedAt\":1700000000000,"
                "\"updatedAt\":1700000001000,\"source\":\"database\"}"});
        }
        co_return result;
    }
};

struct LatestProjectionContext {
    LatestProjectionDb database;
    RecordingLatestRedis redisClient;

    LatestProjectionDb& db() noexcept { return database; }
    RecordingLatestRedis redis() const noexcept { return redisClient; }
};

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

bool has(const std::vector<collector::ProtocolAction>& actions, collector::ProtocolActionKind kind) {
    return std::any_of(actions.begin(), actions.end(),
                       [kind](const auto& action) { return action.kind == kind; });
}

const collector::ProtocolAction& first(const std::vector<collector::ProtocolAction>& actions,
                                collector::ProtocolActionKind kind) {
    const auto current = std::find_if(actions.begin(), actions.end(),
                                      [kind](const auto& action) { return action.kind == kind; });
    if (current == actions.end())
        throw std::runtime_error("expected protocol action was not emitted");
    return *current;
}

std::uint16_t crc16(std::span<const std::uint8_t> bytes) {
    std::uint16_t crc = 0xFFFFU;
    for (const auto byte : bytes) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) != 0 ? static_cast<std::uint16_t>((crc >> 1U) ^ 0xA001U)
                                  : static_cast<std::uint16_t>(crc >> 1U);
    }
    return crc;
}

std::vector<std::uint8_t> slFrame(std::uint8_t functionCode,
                                  std::vector<std::uint8_t> body = {0x39, 0x00, 0x12, 0x34}) {
    std::vector<std::uint8_t> frame{0x7E,
                                    0x7E,
                                    0x01,
                                    0x00,
                                    0x00,
                                    0x00,
                                    0x00,
                                    0x01,
                                    0x00,
                                    0x00,
                                    functionCode,
                                    static_cast<std::uint8_t>(body.size() >> 8U),
                                    static_cast<std::uint8_t>(body.size()),
                                    0x02};
    frame.insert(frame.end(), body.begin(), body.end());
    frame.push_back(0x03);
    const auto crc = crc16(frame);
    frame.push_back(static_cast<std::uint8_t>(crc >> 8U));
    frame.push_back(static_cast<std::uint8_t>(crc));
    return frame;
}

std::vector<std::uint8_t> slMultiFrame(std::uint8_t functionCode, std::uint16_t total,
                                       std::uint16_t sequence, std::vector<std::uint8_t> body) {
    const auto packed = (static_cast<std::uint32_t>(total) << 12U) | sequence;
    body.insert(body.begin(),
                {static_cast<std::uint8_t>(packed >> 16U), static_cast<std::uint8_t>(packed >> 8U),
                 static_cast<std::uint8_t>(packed)});
    auto frame = slFrame(functionCode, std::move(body));
    frame[13] = 0x16;
    const auto crc = crc16(std::span<const std::uint8_t>(frame).first(frame.size() - 2));
    frame[frame.size() - 2] = static_cast<std::uint8_t>(crc >> 8U);
    frame.back() = static_cast<std::uint8_t>(crc);
    return frame;
}

std::vector<std::uint8_t> modbusRead(std::uint16_t transaction, std::uint8_t function = 3) {
    return {static_cast<std::uint8_t>(transaction >> 8U),
            static_cast<std::uint8_t>(transaction),
            0,
            0,
            0,
            6,
            1,
            function,
            0,
            0,
            0,
            1};
}

std::vector<std::uint8_t> modbusReadResponse(std::uint16_t transaction, std::uint8_t function = 3,
                                             std::vector<std::uint8_t> data = {0x12, 0x34}) {
    const auto length = static_cast<std::uint16_t>(3 + data.size());
    std::vector<std::uint8_t> response{static_cast<std::uint8_t>(transaction >> 8U),
                                       static_cast<std::uint8_t>(transaction),
                                       0,
                                       0,
                                       static_cast<std::uint8_t>(length >> 8U),
                                       static_cast<std::uint8_t>(length),
                                       1,
                                       function,
                                       static_cast<std::uint8_t>(data.size())};
    response.insert(response.end(), data.begin(), data.end());
    return response;
}

std::vector<std::uint8_t> modbusWrite(std::uint16_t transaction) {
    return {static_cast<std::uint8_t>(transaction >> 8U),
            static_cast<std::uint8_t>(transaction),
            0,
            0,
            0,
            6,
            1,
            6,
            0,
            0,
            0x12,
            0x34};
}

std::vector<std::uint8_t> withModbusCrc(std::vector<std::uint8_t> frame) {
    const auto crc = crc16(frame);
    frame.push_back(static_cast<std::uint8_t>(crc));
    frame.push_back(static_cast<std::uint8_t>(crc >> 8U));
    return frame;
}

std::vector<std::uint8_t> modbusRequest(bool tcp, std::uint16_t transaction,
                                        std::uint8_t function) {
    std::vector<std::uint8_t> pdu{1, function, 0, 0};
    if (function >= 1 && function <= 4)
        pdu.insert(pdu.end(), {0, 1});
    else if (function == 5)
        pdu.insert(pdu.end(), {0xFF, 0x00});
    else if (function == 6)
        pdu.insert(pdu.end(), {0x12, 0x34});
    else if (function == 15)
        pdu.insert(pdu.end(), {0, 8, 1, 0x01});
    else if (function == 16)
        pdu.insert(pdu.end(), {0, 2, 4, 0x12, 0x34, 0x12, 0x34});
    if (!tcp)
        return withModbusCrc(std::move(pdu));
    const auto length = static_cast<std::uint16_t>(pdu.size());
    std::vector<std::uint8_t> frame{
        static_cast<std::uint8_t>(transaction >> 8U), static_cast<std::uint8_t>(transaction), 0, 0,
        static_cast<std::uint8_t>(length >> 8U),      static_cast<std::uint8_t>(length)};
    frame.insert(frame.end(), pdu.begin(), pdu.end());
    return frame;
}

std::vector<std::uint8_t> modbusResponse(bool tcp, std::uint16_t transaction,
                                         std::uint8_t function) {
    std::vector<std::uint8_t> pdu{1, function};
    if (function == 1 || function == 2)
        pdu.insert(pdu.end(), {1, 0x01});
    else if (function == 3 || function == 4)
        pdu.insert(pdu.end(), {2, 0x12, 0x34});
    else if (function == 5)
        pdu.insert(pdu.end(), {0, 0, 0xFF, 0x00});
    else if (function == 6)
        pdu.insert(pdu.end(), {0, 0, 0x12, 0x34});
    else if (function == 15)
        pdu.insert(pdu.end(), {0, 0, 0, 8});
    else if (function == 16)
        pdu.insert(pdu.end(), {0, 0, 0, 2});
    if (!tcp)
        return withModbusCrc(std::move(pdu));
    const auto length = static_cast<std::uint16_t>(pdu.size());
    std::vector<std::uint8_t> frame{
        static_cast<std::uint8_t>(transaction >> 8U), static_cast<std::uint8_t>(transaction), 0, 0,
        static_cast<std::uint8_t>(length >> 8U),      static_cast<std::uint8_t>(length)};
    frame.insert(frame.end(), pdu.begin(), pdu.end());
    return frame;
}

std::vector<std::uint8_t> modbusRtuRead() { return withModbusCrc({1, 3, 0, 0, 0, 1}); }

std::vector<std::uint8_t> modbusRtuReadResponse() { return withModbusCrc({1, 3, 2, 0x12, 0x34}); }

std::vector<std::uint8_t> modbusRtuWrite() { return withModbusCrc({1, 6, 0, 0, 0x12, 0x34}); }

std::vector<std::uint8_t> modbusReadResponseForRequest(std::span<const std::uint8_t> request,
                                                       bool tcp) {
    const auto functionOffset = tcp ? 7U : 1U;
    const auto quantityOffset = tcp ? 10U : 4U;
    require(request.size() >= quantityOffset + 2 && request[functionOffset] >= 1 &&
                request[functionOffset] <= 4,
            "Modbus poll response helper received an invalid read request");
    const auto function = request[functionOffset];
    const auto quantity = static_cast<std::size_t>(request[quantityOffset] << 8U) |
                          request[quantityOffset + 1];
    const auto byteCount = function <= 2 ? (quantity + 7U) / 8U : quantity * 2U;
    if (!tcp) {
        std::vector<std::uint8_t> response{request[0], function,
                                           static_cast<std::uint8_t>(byteCount)};
        response.resize(response.size() + byteCount, 0);
        return withModbusCrc(std::move(response));
    }
    const auto length = static_cast<std::uint16_t>(3 + byteCount);
    std::vector<std::uint8_t> response{request[0],
                                       request[1],
                                       0,
                                       0,
                                       static_cast<std::uint8_t>(length >> 8U),
                                       static_cast<std::uint8_t>(length),
                                       request[6],
                                       function,
                                       static_cast<std::uint8_t>(byteCount)};
    response.resize(response.size() + byteCount, 0);
    return response;
}

std::vector<collector::ProtocolAction>
drainInitialModbusPoll(collector::ProtocolEngine& engine, std::vector<collector::ProtocolAction> actions,
                       service::message::IngressPacket& packet, bool tcp) {
    while (true) {
        const auto outbound = std::find_if(actions.begin(), actions.end(), [](const auto& action) {
            return action.kind == collector::ProtocolActionKind::Send;
        });
        if (outbound == actions.end())
            return actions;
        packet.payload = modbusReadResponseForRequest(outbound->bytes, tcp);
        actions = engine.consume(packet);
    }
}

std::vector<std::uint8_t> s7ReadRequest(std::uint16_t reference) {
    return {0x03,
            0x00,
            0x00,
            0x1F,
            0x02,
            0xF0,
            0x80,
            0x32,
            0x01,
            0x00,
            0x00,
            static_cast<std::uint8_t>(reference >> 8U),
            static_cast<std::uint8_t>(reference),
            0x00,
            0x0E,
            0x00,
            0x00,
            0x04,
            0x01,
            0x12,
            0x0A,
            0x10,
            0x02,
            0x00,
            0x01,
            0x00,
            0x01,
            0x84,
            0x00,
            0x00,
            0x00};
}

std::vector<std::uint8_t> s7ReadResponse(std::uint16_t reference) {
    return {0x03,
            0x00,
            0x00,
            0x1B,
            0x02,
            0xF0,
            0x80,
            0x32,
            0x03,
            0x00,
            0x00,
            static_cast<std::uint8_t>(reference >> 8U),
            static_cast<std::uint8_t>(reference),
            0x00,
            0x02,
            0x00,
            0x06,
            0x00,
            0x00,
            0x04,
            0x01,
            0xFF,
            0x04,
            0x00,
            0x10,
            0x12,
            0x34};
}

std::vector<std::uint8_t> s7WriteRequest(std::uint16_t reference) {
    return {0x03,
            0x00,
            0x00,
            0x13,
            0x02,
            0xF0,
            0x80,
            0x32,
            0x01,
            0x00,
            0x00,
            static_cast<std::uint8_t>(reference >> 8U),
            static_cast<std::uint8_t>(reference),
            0x00,
            0x02,
            0x00,
            0x00,
            0x05,
            0x01};
}

std::vector<std::uint8_t> s7WriteResponse(std::uint16_t reference) {
    return {0x03,
            0x00,
            0x00,
            0x15,
            0x02,
            0xF0,
            0x80,
            0x32,
            0x03,
            0x00,
            0x00,
            static_cast<std::uint8_t>(reference >> 8U),
            static_cast<std::uint8_t>(reference),
            0x00,
            0x02,
            0x00,
            0x00,
            0x00,
            0x00,
            0x05,
            0x01};
}

std::uint16_t s7Reference(std::span<const std::uint8_t> frame) {
    require(frame.size() >= 13, "S7 frame does not contain a PDU reference");
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(frame[11]) << 8U) | frame[12];
}

std::vector<std::uint8_t> s7CotpConfirm() {
    return {0x03, 0x00, 0x00, 0x0B, 0x06, 0xD0, 0x00, 0x01, 0x00, 0x06, 0x00};
}

std::vector<std::uint8_t> s7SetupResponse(std::uint16_t reference = 0) {
    return {0x03,
            0x00,
            0x00,
            0x1B,
            0x02,
            0xF0,
            0x80,
            0x32,
            0x03,
            0x00,
            0x00,
            static_cast<std::uint8_t>(reference >> 8U),
            static_cast<std::uint8_t>(reference),
            0x00,
            0x08,
            0x00,
            0x00,
            0x00,
            0x00,
            0xF0,
            0x00,
            0x00,
            0x01,
            0x00,
            0x01,
            0x01,
            0xE0};
}

std::vector<std::uint8_t> s7ReadResponseForRequest(std::span<const std::uint8_t> request) {
    require(request.size() >= 31 && request[17] == 0x04,
            "S7 read response helper received an invalid request");
    const auto count = request[18];
    std::vector<std::uint8_t> data;
    for (std::size_t index = 0; index < count; ++index) {
        const auto offset = 19 + index * 12;
        require(offset + 12 <= request.size(), "S7 read request item is truncated");
        const auto amount = static_cast<std::size_t>(request[offset + 4] << 8U) |
                            request[offset + 5];
        const auto wordLength = request[offset + 3];
        const auto byteLength =
            amount * (wordLength == 0x1C || wordLength == 0x1D ? 2U : 1U);
        data.insert(data.end(), {0xFF, 0x04,
                                 static_cast<std::uint8_t>((byteLength * 8U) >> 8U),
                                 static_cast<std::uint8_t>(byteLength * 8U)});
        for (std::size_t byte = 0; byte < byteLength; ++byte)
            data.push_back(index == 0 && byte == 0 ? 0x12 : index == 0 && byte == 1 ? 0x34 : 0);
        if ((byteLength & 1U) != 0 && index + 1 < count)
            data.push_back(0);
    }
    const auto totalLength = static_cast<std::uint16_t>(21 + data.size());
    const auto dataLength = static_cast<std::uint16_t>(data.size());
    std::vector<std::uint8_t> response{0x03,
                                       0x00,
                                       static_cast<std::uint8_t>(totalLength >> 8U),
                                       static_cast<std::uint8_t>(totalLength),
                                       0x02,
                                       0xF0,
                                       0x80,
                                       0x32,
                                       0x03,
                                       0x00,
                                       0x00,
                                       request[11],
                                       request[12],
                                       0x00,
                                       0x02,
                                       static_cast<std::uint8_t>(dataLength >> 8U),
                                       static_cast<std::uint8_t>(dataLength),
                                       0x00,
                                       0x00,
                                       0x04,
                                       count};
    response.insert(response.end(), data.begin(), data.end());
    return response;
}

collector::ProtocolRuntimeRegistry runtimes() {
    collector::ProtocolRuntimeRegistry result;
    result.add(std::make_unique<collector::modbus::Runtime>());
    result.add(std::make_unique<collector::s7::Runtime>());
    result.add(std::make_unique<collector::sl651::Runtime>());
    return result;
}

void testCapabilities() {
    require(collector::DeviceDefinition{}.timezone == "+08:00", "device timezone must default to UTC+8");
    auto registry = runtimes();
    require(registry.require("SL651").capabilities().has(collector::ProtocolCapability::TcpServer),
            "SL651 must support TCP Server");
    require(!registry.require("SL651").capabilities().has(collector::ProtocolCapability::TcpClient),
            "SL651 must reject TCP Client");
    require(!registry.require("SL651").capabilities().has(collector::ProtocolCapability::Polling),
            "SL651 must not expose polling");
    require(!registry.require("SL651").capabilities().has(
                collector::ProtocolCapability::Registration) &&
                !registry.require("SL651").capabilities().has(
                    collector::ProtocolCapability::Heartbeat),
            "SL651 must derive identity from protocol frames without registration or heartbeat");
    require(registry.require("Modbus").capabilities().has(collector::ProtocolCapability::Discovery),
            "Modbus discovery capability missing");
    require(registry.require("S7").capabilities().has(collector::ProtocolCapability::Polling),
            "S7 polling capability missing");
}

void testSl651() {
    collector::RuntimeSnapshot snapshot;
    snapshot.links.push_back({.id = "sl-link",
                              .name = "SL",
                              .mode = "TCP Server",
                              .protocol = "SL651",
                              .ip = "127.0.0.1",
                              .port = 15001,
                              .status = "enabled"});
    collector::DeviceDefinition device;
    device.id = "sl-device";
    device.code = "0000000001";
    device.linkId = "sl-link";
    device.protocol = "SL651";
    device.elements.push_back({.id = "water",
                               .name = "Water",
                               .unit = "m",
                               .functionCode = "32",
                               .guideHex = "3900",
                               .encoding = "BCD",
                               .length = 2,
                               .digits = 2});
    device.elements.push_back({.id = "request-value",
                               .name = "Request value",
                               .functionCode = "4C",
                               .direction = "DOWN",
                               .guideHex = "3900",
                               .encoding = "BCD",
                               .length = 2,
                               .digits = 2});
    device.elements.push_back({.id = "response-value",
                               .name = "Response value",
                               .functionCode = "4C",
                               .direction = "DOWN",
                               .guideHex = "3900",
                               .encoding = "BCD",
                               .length = 2,
                               .digits = 2,
                               .responseElement = true});
    snapshot.devices.push_back(device);

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    (void)engine.connected({.connectionId = "sl-connection",
                            .linkId = "sl-link",
                            .remoteAddress = "127.0.0.1:10001",
                            .sessionEpoch = 1});
    const auto frame =
        slFrame(0x32, {0x00, 0x01, 0x24, 0x01, 0x02, 0x03, 0x04, 0x05, 0x39, 0x00, 0x12, 0x34});
    service::message::IngressPacket packet{.messageId = "sl-ingress",
                                          .linkId = "sl-link",
                                          .connectionId = "sl-connection",
                                          .remoteAddress = "127.0.0.1:10001",
                                          .occurredAtMs = 1000};
    packet.payload.assign(frame.begin(), frame.begin() + 5);
    require(engine.consume(packet).empty(), "partial SL651 header must not emit actions");
    packet.payload.assign(frame.begin() + 5, frame.end());
    const auto actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::BindDevice), "SL651 did not bind device code");
    const auto& parsed = first(actions, collector::ProtocolActionKind::PublishParsed).parsed;
    require(parsed.rawPayloads == std::vector<std::vector<std::uint8_t>>{frame},
            "SL651 raw frame was not preserved as a one-item array");
    require(parsed.valuesJson.find("12.34") != std::string::npos,
            "SL651 BCD element was not parsed");
    require(parsed.observedAtMs == 1704135845000,
            "SL651 device-local report time was not converted from UTC+8 to UTC");

    packet.payload = slFrame(0x4C);
    const auto responseActions = engine.consume(packet);
    const auto& responseParsed =
        first(responseActions, collector::ProtocolActionKind::PublishParsed).parsed;
    require(responseParsed.valuesJson.find("response-value") != std::string::npos &&
                responseParsed.valuesJson.find("request-value") == std::string::npos,
            "SL651 upstream response did not use responseElements");

    auto commandFrame = slFrame(0x4C, {});
    // Downlink direction occupies the high nibble of the length field; refresh the CRC.
    commandFrame[2] = 0x00;
    commandFrame[6] = 0x01;
    commandFrame[11] = 0x80;
    const auto commandCrc =
        crc16(std::span<const std::uint8_t>(commandFrame).first(commandFrame.size() - 2));
    commandFrame[commandFrame.size() - 2] = static_cast<std::uint8_t>(commandCrc >> 8U);
    commandFrame.back() = static_cast<std::uint8_t>(commandCrc);
    auto commandActions = engine.execute("sl-connection", {.id = "sl-command",
                                                           .deviceId = "sl-device",
                                                           .deviceCode = "0000000001",
                                                           .kind = "control",
                                                           .payload = commandFrame});
    require(has(commandActions, collector::ProtocolActionKind::Send), "SL651 command was not sent");
    packet.payload = slFrame(0x32);
    commandActions = engine.consume(packet);
    require(!has(commandActions, collector::ProtocolActionKind::CompleteCommand),
            "SL651 unsolicited report completed a command");
    packet.payload = slFrame(0xE1, {});
    commandActions = engine.consume(packet);
    require(has(commandActions, collector::ProtocolActionKind::CompleteCommand),
            "SL651 ACK did not complete command");

    commandActions = engine.execute(
        "sl-connection", {.id = "sl-element-command",
                          .deviceId = "sl-device",
                          .deviceCode = "0000000001",
                          .kind = "command",
                          .elements = {{.elementId = "request-value", .value = "12.34"}}});
    const auto& generatedSl651 = first(commandActions, collector::ProtocolActionKind::Send).bytes;
    require(generatedSl651[2] == 0x00 && generatedSl651[6] == 0x01 && generatedSl651[7] == 0x01 &&
                generatedSl651[10] == 0x4C,
            "SL651 element command did not use the iot-manager default address header");
    const std::array<std::uint8_t, 4> encodedSl651Value{0x39, 0x00, 0x12, 0x34};
    require(std::search(generatedSl651.begin(), generatedSl651.end(), encodedSl651Value.begin(),
                        encodedSl651Value.end()) != generatedSl651.end(),
            "SL651 element command did not encode its guide and BCD value");
    packet.payload = slFrame(0xE1, {});
    commandActions = engine.consume(packet);
    require(has(commandActions, collector::ProtocolActionKind::CompleteCommand),
            "generated SL651 command did not complete on ACK");

    commandActions = engine.execute("sl-connection", {.id = "sl-negative-command",
                                                      .deviceId = "sl-device",
                                                      .deviceCode = "0000000001",
                                                      .kind = "control",
                                                      .payload = commandFrame});
    require(has(commandActions, collector::ProtocolActionKind::Send),
            "SL651 negative-ack test command was not sent");
    packet.payload = slFrame(0xE2, {});
    commandActions = engine.consume(packet);
    require(first(commandActions, collector::ProtocolActionKind::FailCommand).reason ==
                "sl651_negative_ack",
            "SL651 negative ACK did not fail with a precise reason");
}

void testSl651AllEncodingsAndFunctionCodes() {
    collector::ElementDefinition element;
    element.encoding = "BCD";
    element.digits = 2;
    const std::array<std::uint8_t, 2> bcd{0x12, 0x34};
    require(collector::sl651::detail::elementValue(bcd, element) == "12.34", "SL651 BCD encoding failed");
    element.encoding = "TIME_YYMMDDHHMMSS";
    const std::array<std::uint8_t, 6> time{0x24, 0x01, 0x02, 0x03, 0x04, 0x05};
    require(collector::sl651::detail::elementValue(time, element) == "2024-01-02T03:04:05",
            "SL651 time encoding failed");
    const std::array<std::uint8_t, 3> binary{0x00, 0xAB, 0xFF};
    for (const auto encoding : {"HEX", "DICT"}) {
        element.encoding = encoding;
        require(collector::sl651::detail::elementValue(binary, element) == "00ABFF",
                "SL651 binary encoding failed");
    }
    element.encoding = "JPEG";
    require(collector::sl651::detail::elementValue(binary, element) == "INVALID_JPEG",
            "SL651 accepted an invalid JPEG");
    const std::array<std::uint8_t, 4> jpeg{0xFF, 0xD8, 0xFF, 0xD9};
    require(collector::sl651::detail::elementValue(jpeg, element) == "data:image/jpeg;base64,/9j/2Q==",
            "SL651 JPEG was not stored as a Base64 data URL");

    collector::RuntimeSnapshot snapshot;
    snapshot.links.push_back({.id = "sl-function-link",
                              .name = "SL function matrix",
                              .mode = "TCP Server",
                              .protocol = "SL651",
                              .status = "enabled"});
    collector::DeviceDefinition device;
    device.id = "sl-function-device";
    device.code = "0000000001";
    device.linkId = "sl-function-link";
    device.protocol = "SL651";
    for (std::uint16_t function = 0; function <= 0xFF; ++function) {
        const auto code = collector::sl651::detail::hexByte(static_cast<std::uint8_t>(function));
        device.elements.push_back({.id = "fc-" + code,
                                   .name = "Function " + code,
                                   .functionCode = code,
                                   .guideHex = "3900",
                                   .encoding = "HEX",
                                   .length = 1});
    }
    snapshot.devices.push_back(std::move(device));

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    (void)engine.connected({.connectionId = "sl-function-connection",
                            .linkId = "sl-function-link",
                            .sessionEpoch = 1});
    service::message::IngressPacket packet{.messageId = "sl-function-frame",
                                          .linkId = "sl-function-link",
                                          .connectionId = "sl-function-connection",
                                          .occurredAtMs = 7000};
    for (std::uint16_t function = 0; function <= 0xFF; ++function) {
        const auto code = collector::sl651::detail::hexByte(static_cast<std::uint8_t>(function));
        packet.payload = slFrame(static_cast<std::uint8_t>(function),
                                 {0x39, 0x00, static_cast<std::uint8_t>(function)});
        const auto actions = engine.consume(packet);
        const auto& parsed = first(actions, collector::ProtocolActionKind::PublishParsed).parsed;
        require(parsed.valuesJson.find("fc-" + code) != std::string::npos,
                "SL651 configured function code was not routed");
    }
}

void testSl651MultiPacketImages() {
    collector::RuntimeSnapshot snapshot;
    snapshot.links.push_back({.id = "sl-image-link",
                              .name = "SL image",
                              .mode = "TCP Server",
                              .protocol = "SL651",
                              .status = "enabled"});
    collector::DeviceDefinition device;
    device.id = "sl-image-device";
    device.code = "0000000001";
    device.linkId = "sl-image-link";
    device.protocol = "SL651";
    device.elements.push_back({.id = "image",
                               .name = "Image",
                               .functionCode = "36",
                               .guideHex = "3900",
                               .encoding = "JPEG",
                               .length = 0});
    snapshot.devices.push_back(std::move(device));

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    (void)engine.connected(
        {.connectionId = "sl-image-connection", .linkId = "sl-image-link", .sessionEpoch = 1});
    service::message::IngressPacket packet{.messageId = "sl-image-frame",
                                          .linkId = "sl-image-link",
                                          .connectionId = "sl-image-connection",
                                          .occurredAtMs = 8000};

    const auto firstFrame = slMultiFrame(0x36, 3, 1, {0x00, 0x01, 0x24, 0x01, 0x02});
    const auto secondFrame = slMultiFrame(0x36, 3, 2, {0x03, 0x04, 0x05, 0x39, 0x00, 0xFF});
    packet.payload = secondFrame;
    auto actions = engine.consume(packet);
    require(!has(actions, collector::ProtocolActionKind::PublishParsed),
            "SL651 published an incomplete image");
    const auto firstToken = first(actions, collector::ProtocolActionKind::ScheduleDeadline).deadlineToken;

    packet.payload = firstFrame;
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CancelDeadline),
            "SL651 did not cancel the previous image idle deadline");
    const auto secondToken = first(actions, collector::ProtocolActionKind::ScheduleDeadline).deadlineToken;
    require(secondToken != firstToken, "SL651 did not refresh the image idle deadline");
    require(engine.deadline("sl-image-connection", firstToken).empty(),
            "a stale SL651 image deadline affected the refreshed assembly");

    const auto thirdFrame = slMultiFrame(0x36, 3, 3, {0xD8, 0xFF, 0xD9});
    packet.payload = thirdFrame;
    actions = engine.consume(packet);
    const auto& parsed = first(actions, collector::ProtocolActionKind::PublishParsed).parsed;
    require(parsed.valuesJson.find("data:image/jpeg;base64,/9j/2Q==") != std::string::npos,
            "SL651 multi-packet image was not assembled and encoded");
    require(parsed.valuesJson.find("\"type\":\"JPEG\"") != std::string::npos,
            "SL651 image storage omitted the JPEG type");
    require(parsed.valuesJson.find("\"is_multi_packet\":true") != std::string::npos &&
                parsed.valuesJson.find("\"total_packets\":3") != std::string::npos,
            "SL651 image storage omitted multi-packet metadata");
    const std::vector<std::vector<std::uint8_t>> expectedRaw{firstFrame, secondFrame, thirdFrame};
    require(parsed.rawPayloads == expectedRaw,
            "SL651 multi-packet raw frames were not stored in sequence order");
    const auto streamFields = service::message::parsedFields(parsed);
    service::message::StreamMessage streamMessage{.id = "1-0", .fields = streamFields};
    const auto roundTrip = service::message::parsedFrom(streamMessage);
    require(roundTrip.rawPayloads == expectedRaw &&
                streamMessage.get("raw_payload_hex") ==
                    service::message::rawPayloadsJson(expectedRaw),
            "parsed Redis message did not preserve the ordered HEX payload array");

    const auto oldFirst = slMultiFrame(0x36, 2, 1, {0x00, 0x01, 0x24, 0x01});
    packet.payload = oldFirst;
    actions = engine.consume(packet);
    const auto oldToken = first(actions, collector::ProtocolActionKind::ScheduleDeadline).deadlineToken;
    const auto newFirst = slMultiFrame(
        0x36, 2, 1, {0x00, 0x01, 0x24, 0x01, 0x02, 0x03, 0x04, 0x05, 0x39, 0x00, 0xFF, 0xD8});
    packet.payload = newFirst;
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::CancelDeadline).deadlineToken == oldToken,
            "a new SL651 image did not replace the previous partial image");
    require(engine.deadline("sl-image-connection", oldToken).empty(),
            "the replaced SL651 image deadline affected the new image");
    const auto newLast = slMultiFrame(0x36, 2, 2, {0xFF, 0xD9});
    packet.payload = newLast;
    actions = engine.consume(packet);
    const auto& replacement = first(actions, collector::ProtocolActionKind::PublishParsed).parsed;
    const std::vector<std::vector<std::uint8_t>> replacementRaw{newFirst, newLast};
    require(replacement.rawPayloads == replacementRaw,
            "the replaced SL651 image leaked old raw packets into storage");
}

void testModbus() {
    collector::RuntimeSnapshot snapshot;
    collector::LinkDefinition link{.id = "modbus-link",
                            .name = "Modbus",
                            .mode = "TCP Client",
                            .protocol = "Modbus",
                            .status = "enabled"};
    link.targets.push_back({.id = "target-1",
                            .name = "Target",
                            .ip = "127.0.0.1",
                            .port = 15002,
                            .status = "enabled"});
    snapshot.links.push_back(link);
    collector::DeviceDefinition device;
    device.id = "modbus-device";
    device.code = "MODBUS-1";
    device.linkId = "modbus-link";
    device.linkMode = "TCP Client";
    device.targetId = "target-1";
    device.protocol = "Modbus";
    device.modbusMode = "TCP";
    device.slaveId = 1;
    device.elements.push_back({.id = "holding-0",
                               .name = "Holding",
                               .unit = "V",
                               .dataType = "UINT16",
                               .byteOrder = "BIG_ENDIAN",
                               .registerType = "HOLDING_REGISTER",
                               .address = 0,
                               .quantity = 1,
                               .writable = true});
    device.elements.push_back({.id = "holding-2",
                               .name = "Holding 2",
                               .dataType = "UINT16",
                               .byteOrder = "BIG_ENDIAN",
                               .registerType = "HOLDING_REGISTER",
                               .address = 2,
                               .quantity = 1});
    snapshot.devices.push_back(device);

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    auto connected = engine.connected({.connectionId = "modbus-connection",
                                       .linkId = "modbus-link",
                                       .remoteAddress = "127.0.0.1:15002",
                                       .targetId = "target-1",
                                       .sessionEpoch = 1});
    require(has(connected, collector::ProtocolActionKind::BindDevice),
            "Modbus client target was not bound");
    require(has(connected, collector::ProtocolActionKind::Send),
            "Modbus did not trigger the first poll immediately after binding");
    service::message::IngressPacket packet{.messageId = "modbus-ingress",
                                          .linkId = "modbus-link",
                                          .connectionId = "modbus-connection",
                                          .remoteAddress = "127.0.0.1:15002",
                                          .occurredAtMs = 2000};
    connected = drainInitialModbusPoll(engine, std::move(connected), packet, true);
    const auto pollToken = first(connected, collector::ProtocolActionKind::ScheduleDeadline).deadlineToken;

    auto actions = engine.execute("modbus-connection", {.id = "modbus-read",
                                                        .deviceId = "modbus-device",
                                                         .deviceCode = "MODBUS-1",
                                                         .kind = "read",
                                                         .payload = modbusRead(1)});
    require(has(actions, collector::ProtocolActionKind::Send), "Modbus read was not dispatched");
    packet.payload = modbusReadResponse(1);
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::PublishParsed), "Modbus response was not parsed");
    require(has(actions, collector::ProtocolActionKind::CompleteCommand), "Modbus read did not complete");

    actions = engine.execute("modbus-connection", {.id = "invalid-write",
                                                   .deviceId = "modbus-device",
                                                   .deviceCode = "MODBUS-1",
                                                   .kind = "write",
                                                   .payload = modbusWrite(2)});
    require(first(actions, collector::ProtocolActionKind::FailCommand).reason ==
                "modbus_write_readback_required",
            "Modbus accepted a write without readback");

    actions = engine.execute("modbus-connection", {.id = "verified-write",
                                                   .deviceId = "modbus-device",
                                                   .deviceCode = "MODBUS-1",
                                                   .kind = "write",
                                                   .payload = modbusWrite(3),
                                                   .readbackPayload = modbusRead(4),
                                                   .expectedReadbackData = {0x12, 0x34}});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusWrite(3),
            "Modbus write was not sent first");
    packet.payload = modbusWrite(3);
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusRead(4),
            "Modbus write response did not trigger readback");
    packet.payload = modbusReadResponse(4);
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "Modbus verified write did not complete");

    actions = engine.execute("modbus-connection",
                             {.id = "element-write",
                              .deviceId = "modbus-device",
                              .deviceCode = "MODBUS-1",
                              .kind = "command",
                              .elements = {{.elementId = "holding-0", .value = "4660"}}});
    const auto generatedWrite = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(generatedWrite.size() == 12 && generatedWrite[7] == 6 &&
                generatedWrite[10] == 0x12 && generatedWrite[11] == 0x34,
            "Modbus element command did not compile FC06");
    packet.payload = generatedWrite;
    actions = engine.consume(packet);
    const auto generatedReadback = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(generatedReadback.size() == 12 && generatedReadback[7] == 3,
            "Modbus element command did not compile FC03 readback");
    const auto readbackTransaction = static_cast<std::uint16_t>(generatedReadback[0] << 8U) |
                                     generatedReadback[1];
    packet.payload = modbusReadResponse(readbackTransaction);
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "Modbus element command did not complete after readback");

    actions = engine.deadline("modbus-connection", pollToken);
    require(has(actions, collector::ProtocolActionKind::Send),
            "Modbus periodic poll was not generated by its session");
    const auto& poll = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(poll.size() == 12 && poll[7] == 3 && poll[10] == 0 && poll[11] == 3,
            "Modbus register mergeGap did not combine one read range");
}

void testModbusTypesAndPriority() {
    collector::ElementDefinition element;
    element.dataType = "UINT64";
    element.byteOrder = "BIG_ENDIAN";
    const std::array<std::uint8_t, 8> maximum{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    require(collector::modbus::detail::numericJson(maximum, element) == "18446744073709551615",
            "Modbus UINT64 lost integer precision");
    element.dataType = "UINT32";
    element.byteOrder = "BIG_ENDIAN_BYTE_SWAP";
    const std::array<std::uint8_t, 4> bigSwap{0x34, 0x12, 0x78, 0x56};
    require(collector::modbus::detail::numericJson(bigSwap, element) == "305419896",
            "Modbus BIG_ENDIAN_BYTE_SWAP was decoded incorrectly");
    element.byteOrder = "LITTLE_ENDIAN_BYTE_SWAP";
    const std::array<std::uint8_t, 4> littleSwap{0x56, 0x78, 0x12, 0x34};
    require(collector::modbus::detail::numericJson(littleSwap, element) == "305419896",
            "Modbus LITTLE_ENDIAN_BYTE_SWAP was decoded incorrectly");

    collector::RuntimeSnapshot snapshot;
    collector::LinkDefinition link{.id = "priority-link",
                            .name = "Priority",
                            .mode = "TCP Client",
                            .protocol = "Modbus",
                            .status = "enabled"};
    link.targets.push_back({.id = "priority-target",
                            .name = "Target",
                            .ip = "127.0.0.1",
                            .port = 15004,
                            .status = "enabled"});
    snapshot.links.push_back(link);
    collector::DeviceDefinition device;
    device.id = "priority-device";
    device.code = "PRIORITY-1";
    device.linkId = link.id;
    device.linkMode = link.mode;
    device.targetId = "priority-target";
    device.protocol = "Modbus";
    device.modbusMode = "TCP";
    device.elements.push_back({.id = "coil",
                               .name = "Coil",
                               .dataType = "BOOL",
                               .registerType = "COIL",
                               .address = 0,
                               .quantity = 1});
    snapshot.devices.push_back(device);

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    auto connected = engine.connected({.connectionId = "priority-connection",
                                       .linkId = link.id,
                                       .targetId = "priority-target",
                                       .sessionEpoch = 1});
    service::message::IngressPacket packet{.messageId = "initial-poll-response",
                                          .linkId = link.id,
                                          .connectionId = "priority-connection",
                                          .occurredAtMs = 4999};
    (void)drainInitialModbusPoll(engine, std::move(connected), packet, true);
    auto actions = engine.execute("priority-connection", {.id = "active-read",
                                                          .deviceId = device.id,
                                                          .deviceCode = device.code,
                                                          .kind = "read",
                                                          .payload = modbusRead(10, 1)});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusRead(10, 1),
            "Modbus active read was not dispatched");
    actions = engine.execute("priority-connection", {.id = "normal-read",
                                                     .deviceId = device.id,
                                                     .deviceCode = device.code,
                                                     .kind = "read",
                                                     .payload = modbusRead(11, 1),
                                                     .highPriority = false});
    require(actions.empty(), "Modbus normal read bypassed the in-flight request");
    actions = engine.execute("priority-connection", {.id = "high-write",
                                                     .deviceId = device.id,
                                                     .deviceCode = device.code,
                                                     .kind = "write",
                                                     .payload = modbusWrite(12),
                                                     .readbackPayload = modbusRead(13),
                                                     .expectedReadbackData = {0x12, 0x34},
                                                     .highPriority = true});
    require(actions.empty(), "Modbus high write bypassed the in-flight request");
    packet.messageId = "active-response";
    packet.occurredAtMs = 5000;
    packet.payload = modbusReadResponse(10, 1, {0x01});
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::PublishParsed)
                    .parsed.valuesJson.find("\"value\":1") != std::string::npos,
            "Modbus coil response was not parsed bitwise");
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusWrite(12),
            "Modbus high-priority write did not jump ahead of a normal read");
    packet.payload = modbusWrite(12);
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusRead(13),
            "Modbus high-priority write did not enter atomic readback");
    packet.payload = modbusReadResponse(13);
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusRead(11, 1),
            "Modbus normal read was not resumed after write readback");
}

void testModbusAllDataTypesAndByteOrders() {
    struct TypeCase {
        std::string_view type;
        std::vector<std::uint8_t> canonical;
        std::string_view expected;
    };
    const std::array cases{
        TypeCase{"BOOL", {0x01}, "1"},
        TypeCase{"INT16", {0xFF, 0xFE}, "-2"},
        TypeCase{"UINT16", {0x12, 0x34}, "4660"},
        TypeCase{"INT32", {0xFF, 0xFF, 0xFF, 0xFE}, "-2"},
        TypeCase{"UINT32", {0x12, 0x34, 0x56, 0x78}, "305419896"},
        TypeCase{"FLOAT32", {0x3F, 0xC0, 0x00, 0x00}, "1.5"},
        TypeCase{"INT64", {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE}, "-2"},
        TypeCase{"UINT64", {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}, "81985529216486895"},
        TypeCase{"DOUBLE", {0x3F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, "1.5"},
    };
    const std::array<std::string_view, 4> orders{"BIG_ENDIAN", "LITTLE_ENDIAN",
                                                 "BIG_ENDIAN_BYTE_SWAP", "LITTLE_ENDIAN_BYTE_SWAP"};
    for (const auto& current : cases) {
        collector::ElementDefinition element;
        element.dataType = current.type;
        for (const auto order : orders) {
            element.byteOrder = order;
            const auto wire = collector::modbus::detail::orderedBytes(current.canonical, order);
            const auto decoded = collector::modbus::detail::numericJson(wire, element);
            require(decoded && *decoded == current.expected,
                    "Modbus data type or byte order matrix failed");
        }
    }

    collector::ElementDefinition scaled;
    scaled.dataType = "UINT16";
    scaled.byteOrder = "BIG_ENDIAN";
    scaled.scale = 0.1;
    scaled.decimals = 1;
    const std::array<std::uint8_t, 2> raw{0x04, 0xD2};
    require(collector::modbus::detail::numericJson(raw, scaled) == "123.4",
            "Modbus scale and decimals were not applied");
}

void testModbusAllFunctionCodes(bool tcp) {
    collector::RuntimeSnapshot snapshot;
    collector::LinkDefinition link{.id = tcp ? "modbus-tcp-matrix" : "modbus-rtu-matrix",
                            .name = "Modbus function matrix",
                            .mode = "TCP Client",
                            .protocol = "Modbus",
                            .status = "enabled"};
    link.targets.push_back({.id = "matrix-target",
                            .name = "Matrix target",
                            .ip = "127.0.0.1",
                            .port = 15005,
                            .status = "enabled"});
    snapshot.links.push_back(link);
    collector::DeviceDefinition device;
    device.id = tcp ? "modbus-tcp-device" : "modbus-rtu-device-matrix";
    device.code = tcp ? "MODBUS-TCP-MATRIX" : "MODBUS-RTU-MATRIX";
    device.linkId = link.id;
    device.linkMode = link.mode;
    device.targetId = "matrix-target";
    device.protocol = "Modbus";
    device.modbusMode = tcp ? "TCP" : "RTU";
    device.slaveId = 1;
    device.elements = {
        {.id = "coil", .name = "Coil", .dataType = "BOOL", .registerType = "COIL", .quantity = 1},
        {.id = "discrete",
         .name = "Discrete",
         .dataType = "BOOL",
         .registerType = "DISCRETE_INPUT",
         .quantity = 1},
        {.id = "holding",
         .name = "Holding",
         .dataType = "UINT16",
         .registerType = "HOLDING_REGISTER",
         .quantity = 1},
        {.id = "input",
         .name = "Input",
         .dataType = "UINT16",
         .registerType = "INPUT_REGISTER",
         .quantity = 1},
    };
    snapshot.devices.push_back(device);

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    service::message::IngressPacket packet{.messageId = "modbus-matrix-response",
                                          .linkId = link.id,
                                          .connectionId = "modbus-matrix-connection",
                                          .occurredAtMs = 6000};
    auto connected = engine.connected({.connectionId = "modbus-matrix-connection",
                                       .linkId = link.id,
                                       .targetId = "matrix-target",
                                       .sessionEpoch = 1});
    (void)drainInitialModbusPoll(engine, std::move(connected), packet, tcp);
    std::uint16_t transaction = 100;
    for (const auto function : std::array<std::uint8_t, 8>{1, 2, 3, 4, 5, 6, 15, 16}) {
        const auto request = modbusRequest(tcp, transaction, function);
        collector::ProtocolCommand command{
            .id = "fc-" + std::to_string(function),
            .deviceId = device.id,
            .deviceCode = device.code,
            .kind = function == 5 || function == 6 || function == 15 || function == 16 ? "write"
                                                                                       : "read",
            .payload = request};
        const auto write = command.kind == "write";
        std::uint8_t readFunction = 0;
        std::uint16_t readTransaction = 0;
        if (write) {
            readFunction = function == 5 || function == 15 ? 1 : 3;
            readTransaction = ++transaction;
            command.readbackPayload = modbusRequest(tcp, readTransaction, readFunction);
            command.expectedReadbackData = readFunction == 1
                                               ? std::vector<std::uint8_t>{0x01}
                                               : std::vector<std::uint8_t>{0x12, 0x34};
        }
        auto actions = engine.execute("modbus-matrix-connection", std::move(command));
        require(first(actions, collector::ProtocolActionKind::Send).bytes == request,
                "Modbus function code request was not dispatched");
        packet.payload = modbusResponse(tcp, transaction - (write ? 1 : 0), function);
        actions = engine.consume(packet);
        if (write) {
            require(first(actions, collector::ProtocolActionKind::Send).bytes ==
                        modbusRequest(tcp, readTransaction, readFunction),
                    "Modbus write function did not trigger readback");
            packet.payload = modbusResponse(tcp, readTransaction, readFunction);
            actions = engine.consume(packet);
        }
        require(has(actions, collector::ProtocolActionKind::CompleteCommand),
                "Modbus function code did not complete");
        ++transaction;
    }
}

void testModbusRtuZeroAddress() {
    collector::RuntimeSnapshot snapshot;
    collector::LinkDefinition link{.id = "modbus-rtu-link",
                            .name = "Modbus RTU",
                            .mode = "TCP Client",
                            .protocol = "Modbus",
                            .status = "enabled"};
    link.targets.push_back({.id = "rtu-target",
                            .name = "RTU Target",
                            .ip = "127.0.0.1",
                            .port = 15003,
                            .status = "enabled"});
    snapshot.links.push_back(link);
    collector::DeviceDefinition device;
    device.id = "modbus-rtu-device";
    device.code = "MODBUS-RTU-1";
    device.linkId = link.id;
    device.linkMode = link.mode;
    device.targetId = "rtu-target";
    device.protocol = "Modbus";
    device.modbusMode = "RTU";
    device.slaveId = 1;
    device.elements.push_back({.id = "rtu-holding-0",
                               .name = "Holding",
                               .dataType = "UINT16",
                               .registerType = "HOLDING_REGISTER",
                               .address = 0,
                               .quantity = 1});
    snapshot.devices.push_back(device);

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    auto connected = engine.connected({.connectionId = "modbus-rtu-connection",
                                       .linkId = link.id,
                                       .targetId = "rtu-target",
                                       .sessionEpoch = 1});
    service::message::IngressPacket packet{.messageId = "rtu-initial-poll-response",
                                          .linkId = link.id,
                                          .connectionId = "modbus-rtu-connection",
                                          .occurredAtMs = 2999};
    (void)drainInitialModbusPoll(engine, std::move(connected), packet, false);
    auto actions = engine.execute("modbus-rtu-connection", {.id = "rtu-read",
                                                            .deviceId = device.id,
                                                            .deviceCode = device.code,
                                                            .kind = "read",
                                                            .payload = modbusRtuRead()});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusRtuRead(),
            "zero-address Modbus RTU read was mistaken for TCP");
    packet.messageId = "rtu-read-response";
    packet.occurredAtMs = 3000;
    packet.payload = modbusRtuReadResponse();
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "zero-address Modbus RTU response did not complete");

    actions = engine.execute("modbus-rtu-connection", {.id = "rtu-write",
                                                       .deviceId = device.id,
                                                       .deviceCode = device.code,
                                                       .kind = "write",
                                                       .payload = modbusRtuWrite(),
                                                       .readbackPayload = modbusRtuRead(),
                                                       .expectedReadbackData = {0x12, 0x34}});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusRtuWrite(),
            "zero-address Modbus RTU write was mistaken for TCP");
    packet.messageId = "rtu-write-response";
    packet.payload = modbusRtuWrite();
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::Send).bytes == modbusRtuRead(),
            "Modbus RTU write did not trigger readback");
    packet.messageId = "rtu-readback-response";
    packet.payload = modbusRtuReadResponse();
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "Modbus RTU write readback did not complete");
}

void testModbusDiscoveryAndOffline() {
    collector::RuntimeSnapshot snapshot;
    snapshot.links.push_back({.id = "modbus-server",
                              .name = "Modbus Server",
                              .mode = "TCP Server",
                              .protocol = "Modbus",
                              .status = "enabled"});
    for (std::uint8_t unit = 1; unit <= 2; ++unit) {
        collector::DeviceDefinition device;
        device.id = "discovery-device-" + std::to_string(unit);
        device.code = "DISCOVERY" + std::to_string(unit);
        device.linkId = "modbus-server";
        device.linkMode = "TCP Server";
        device.protocol = "Modbus";
        device.modbusMode = "TCP";
        device.slaveId = unit;
        device.onlineTimeout = 1;
        snapshot.devices.push_back(std::move(device));
    }
    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    (void)engine.connected(
        {.connectionId = "discovery-connection", .linkId = "modbus-server", .sessionEpoch = 1});
    auto actions =
        engine.execute("discovery-connection", {.id = "discovery-command",
                                                .transport = "TCP",
                                                .kind = "discovery",
                                                .payload = modbusRead(11),
                                                .timeout = std::chrono::milliseconds(500)});
    const auto discoveryToken =
        first(actions, collector::ProtocolActionKind::ScheduleDeadline).deadlineToken;
    service::message::IngressPacket packet{.messageId = "discovery-response-1",
                                          .linkId = "modbus-server",
                                          .connectionId = "discovery-connection",
                                          .occurredAtMs = 4000,
                                          .payload = modbusReadResponse(11)};
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::BindDevice).deviceCode == "DISCOVERY1",
            "Modbus discovery did not register the first response");
    packet.messageId = "discovery-response-2";
    packet.payload[6] = 2;
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::BindDevice).deviceCode == "DISCOVERY2",
            "Modbus discovery did not register the second response");
    actions = engine.deadline("discovery-connection", discoveryToken);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "Modbus discovery window did not complete");

    collector::RuntimeSnapshot registrationSnapshot;
    registrationSnapshot.links = snapshot.links;
    auto registeredDevice = snapshot.devices.front();
    registeredDevice.registrationMode = "ASCII";
    registeredDevice.registrationBytes = {'R', 'E', 'G'};
    registrationSnapshot.devices.push_back(registeredDevice);
    auto conflictingDevice = registeredDevice;
    conflictingDevice.id = "registration-conflict";
    conflictingDevice.code = "REGISTRATION-CONFLICT";
    conflictingDevice.slaveId = 2;
    conflictingDevice.registrationBytes = {'B', 'A', 'D'};
    registrationSnapshot.devices.push_back(conflictingDevice);
    engine.reload(registrationSnapshot);
    (void)engine.connected(
        {.connectionId = "registration-connection", .linkId = "modbus-server", .sessionEpoch = 2});
    packet.connectionId = "registration-connection";
    packet.payload = {'R', 'E', 'G'};
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::BindDevice),
            "Modbus standalone registration packet did not bind the device");
    require(!has(actions, collector::ProtocolActionKind::ScheduleDeadline),
            "Modbus incorrectly applied a device-online timeout to the persistent DTU socket");
    actions = engine.consume(packet);
    require(!has(actions, collector::ProtocolActionKind::Close),
            "Modbus repeated registration packet closed its own DTU connection");
    packet.payload = {'B', 'A', 'D'};
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::Close).reason ==
                "modbus_registration_conflict",
            "Modbus conflicting registration was not rejected");

    (void)engine.connected(
        {.connectionId = "prefixed-registration", .linkId = "modbus-server", .sessionEpoch = 3});
    packet.connectionId = "prefixed-registration";
    packet.payload = {'R', 'E', 'G'};
    const auto prefixedResponse = modbusReadResponse(11);
    packet.payload.insert(packet.payload.end(), prefixedResponse.begin(), prefixedResponse.end());
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::BindDevice) &&
                !has(actions, collector::ProtocolActionKind::Close),
            "Modbus registration prefix did not preserve the payload after binding");
}

void testS7() {
    collector::RuntimeSnapshot snapshot;
    collector::LinkDefinition link{
        .id = "s7-link", .name = "S7", .mode = "TCP Client", .protocol = "S7", .status = "enabled"};
    link.targets.push_back(
        {.id = "target-1", .name = "PLC", .ip = "127.0.0.1", .port = 102, .status = "enabled"});
    snapshot.links.push_back(link);
    collector::DeviceDefinition device;
    device.id = "s7-device";
    device.code = "S7-1";
    device.linkId = "s7-link";
    device.linkMode = "TCP Client";
    device.targetId = "target-1";
    device.protocol = "S7";
    device.elements.push_back({.id = "db1-word",
                               .name = "DB1 Word",
                               .unit = "",
                               .dataType = "UINT16",
                               .area = "DB",
                               .dbNumber = 1,
                               .start = 0,
                               .size = 2,
                               .writable = true});
    device.elements.push_back({.id = "v-lreal",
                               .name = "V LREAL",
                               .dataType = "LREAL",
                               .area = "V",
                               .start = 4,
                               .size = 8});
    device.elements.push_back({.id = "marker-float",
                               .name = "Marker float",
                               .dataType = "FLOAT",
                               .area = "MK",
                               .start = 0,
                               .size = 4});
    device.elements.push_back({.id = "input-bit",
                               .name = "Input bit",
                               .dataType = "BOOL",
                               .area = "PE",
                               .start = 2,
                               .startBit = 3,
                               .size = 1});
    device.elements.push_back({.id = "output-bit",
                               .name = "Output bit",
                               .dataType = "BOOL",
                               .area = "PA",
                               .start = 3,
                               .startBit = 2,
                               .size = 1,
                               .writable = true});
    device.elements.push_back({.id = "counter",
                               .name = "Counter",
                               .dataType = "UINT16",
                               .area = "CT",
                               .start = 1,
                               .size = 2});
    device.elements.push_back({.id = "timer",
                               .name = "Timer",
                               .dataType = "UINT16",
                               .area = "TM",
                               .start = 1,
                               .size = 2});
    snapshot.devices.push_back(device);

    const std::vector<std::uint8_t> expectedCotp{0x03, 0x00, 0x00, 0x16, 0x11, 0xE0,
                                                 0x00, 0x00, 0x00, 0x01, 0x00, 0xC0,
                                                 0x01, 0x0A, 0xC1, 0x02, 0x01, 0x00,
                                                 0xC2, 0x02, 0x01, 0x01};
    const std::vector<std::uint8_t> expectedSetup{0x03, 0x00, 0x00, 0x19, 0x02, 0xF0, 0x80,
                                                  0x32, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                  0x08, 0x00, 0x00, 0xF0, 0x00, 0x00, 0x01,
                                                  0x00, 0x01, 0x01, 0xE0};
    const std::vector<std::uint8_t> expectedDisconnect{0x03, 0x00, 0x00, 0x0B, 0x06, 0x80,
                                                       0x00, 0x06, 0x00, 0x01, 0x00};

    collector::ProtocolEngine engine(runtimes());
    engine.reload(snapshot);
    service::message::IngressPacket packet{.messageId = "s7-ingress",
                                          .linkId = "s7-link",
                                          .connectionId = "s7-connection",
                                          .remoteAddress = "127.0.0.1:102",
                                          .occurredAtMs = 3000};
    const auto finishHandshake = [&]() {
        packet.payload = s7CotpConfirm();
        auto handshake = engine.consume(packet);
        require(first(handshake, collector::ProtocolActionKind::Send).bytes == expectedSetup,
                "S7 Setup Communication packet does not match iot-manager");
        require(!has(handshake, collector::ProtocolActionKind::ScheduleDeadline),
                "S7 handshake timeout was incorrectly restarted after ISO-CC");
        packet.payload = s7SetupResponse();
        return engine.consume(packet);
    };

    auto actions = engine.connected({.connectionId = "s7-connection",
                                     .linkId = "s7-link",
                                     .remoteAddress = "127.0.0.1:102",
                                     .targetId = "target-1",
                                     .sessionEpoch = 1});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedCotp,
            "S7 ISO-CR packet does not match iot-manager");
    require(first(actions, collector::ProtocolActionKind::ScheduleDeadline).deadlineAfter ==
                std::chrono::seconds(5),
            "S7 handshake timeout does not match iot-manager");
    require(!has(actions, collector::ProtocolActionKind::BindDevice),
            "S7 device became online before protocol negotiation");

    actions = finishHandshake();
    require(has(actions, collector::ProtocolActionKind::BindDevice),
            "S7 device was not bound after protocol negotiation");
    const auto immediatePoll = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(immediatePoll[17] == 0x04,
            "S7 did not trigger the first poll immediately after registration");
    require(immediatePoll.size() == 19 + 7 * 12 && immediatePoll[18] == 7,
            "S7 initial poll did not batch elements within the negotiated PDU");
    packet.payload = s7ReadResponseForRequest(immediatePoll);
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::PublishParsed),
            "S7 immediate poll response was not parsed");
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedDisconnect,
            "S7 did not send the iot-manager ISO-DR packet after polling");
    const auto pollToken = first(actions, collector::ProtocolActionKind::ScheduleDeadline).deadlineToken;

    actions = engine.execute("s7-connection", {.id = "s7-read",
                                               .deviceId = "s7-device",
                                               .deviceCode = "S7-1",
                                               .kind = "read",
                                               .payload = s7ReadRequest(99)});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedCotp,
            "S7 active read did not open a fresh PLC session");
    actions = finishHandshake();
    require(first(actions, collector::ProtocolActionKind::Send).bytes == s7ReadRequest(1),
            "S7 active read PDU sequence does not match iot-manager");
    packet.payload = s7ReadResponse(1);
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::PublishParsed), "S7 response was not parsed");
    require(has(actions, collector::ProtocolActionKind::CompleteCommand), "S7 read did not complete");
    require(first(actions, collector::ProtocolActionKind::PublishParsed).parsed.valuesJson.find("4660") !=
                std::string::npos,
            "S7 DB value was not decoded");
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedDisconnect,
            "S7 read did not close only the PLC session");

    actions = engine.execute("s7-connection", {.id = "s7-write",
                                               .deviceId = "s7-device",
                                               .deviceCode = "S7-1",
                                               .kind = "write",
                                               .payload = s7WriteRequest(77),
                                               .readbackPayload = s7ReadRequest(78),
                                               .expectedReadbackData = {0x12, 0x34}});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedCotp,
            "S7 write did not open a fresh PLC session");
    actions = finishHandshake();
    require(first(actions, collector::ProtocolActionKind::Send).bytes == s7WriteRequest(1),
            "S7 Write Var PDU sequence does not match iot-manager");
    packet.payload = s7WriteResponse(1);
    actions = engine.consume(packet);
    require(first(actions, collector::ProtocolActionKind::Send).bytes == s7ReadRequest(2),
            "S7 Write Var did not trigger same-session Read Var verification");
    packet.payload = s7ReadResponse(2);
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "S7 Write Var readback did not complete");
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedDisconnect,
            "S7 write verification did not finish with ISO-DR");

    actions =
        engine.execute("s7-connection", {.id = "s7-element-write",
                                         .deviceId = "s7-device",
                                         .deviceCode = "S7-1",
                                         .kind = "command",
                                         .elements = {{.elementId = "db1-word", .value = "4660"}}});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedCotp,
            "S7 element write did not open a fresh PLC session");
    actions = finishHandshake();
    const auto generatedWrite = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(generatedWrite.size() == 37 && generatedWrite[17] == 0x05 &&
                generatedWrite[35] == 0x12 && generatedWrite[36] == 0x34 &&
                s7Reference(generatedWrite) == 1,
            "S7 element command did not compile Write Var");
    packet.payload = s7WriteResponse(1);
    actions = engine.consume(packet);
    const auto generatedReadback = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(generatedReadback[17] == 0x04 && s7Reference(generatedReadback) == 2,
            "S7 element command did not compile Read Var verification");
    packet.payload = s7ReadResponse(2);
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "S7 element command did not complete after readback");

    actions = engine.execute(
        "s7-connection", {.id = "s7-bool-write",
                          .deviceId = "s7-device",
                          .deviceCode = "S7-1",
                          .kind = "command",
                          .elements = {{.elementId = "output-bit", .value = "1"}}});
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedCotp,
            "S7 BOOL write did not open a fresh PLC session");
    actions = finishHandshake();
    const auto prepareRead = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(prepareRead[17] == 0x04 && prepareRead[22] == 0x02 &&
                s7Reference(prepareRead) == 1,
            "S7 BOOL write did not read the containing byte first");
    packet.payload = s7ReadResponseForRequest(prepareRead);
    actions = engine.consume(packet);
    const auto boolWrite = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(boolWrite[17] == 0x05 && boolWrite[22] == 0x02 && boolWrite.back() == 0x16 &&
                s7Reference(boolWrite) == 2,
            "S7 BOOL write did not preserve adjacent bits during read-modify-write");
    packet.payload = s7WriteResponse(2);
    actions = engine.consume(packet);
    const auto boolReadback = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(boolReadback[17] == 0x04 && boolReadback[22] == 0x02 &&
                s7Reference(boolReadback) == 3,
            "S7 BOOL write did not start same-session byte readback");
    packet.payload = s7ReadResponseForRequest(boolReadback);
    packet.payload.back() = 0x16;
    actions = engine.consume(packet);
    require(has(actions, collector::ProtocolActionKind::CompleteCommand),
            "S7 BOOL write did not complete after bit-level readback verification");

    actions = engine.deadline("s7-connection", pollToken);
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedCotp,
            "S7 periodic poll did not open a fresh PLC session");
    actions = finishHandshake();
    const auto& batched = first(actions, collector::ProtocolActionKind::Send).bytes;
    require(batched.size() == 19 + 7 * 12 && batched[18] == 7,
            "S7 periodic poll did not batch elements within the negotiated PDU");
    std::map<std::uint8_t, std::size_t> areaCounts;
    std::map<std::uint8_t, std::uint8_t> wordLengths;
    for (std::size_t index = 0; index < batched[18]; ++index) {
        const auto offset = 19 + index * 12;
        ++areaCounts[batched[offset + 8]];
        wordLengths[batched[offset + 8]] = batched[offset + 3];
    }
    require(areaCounts[0x84] == 2 && areaCounts[0x83] == 1 && areaCounts[0x81] == 1 &&
                areaCounts[0x82] == 1 && areaCounts[0x1C] == 1 && areaCounts[0x1D] == 1,
            "S7 area mapping matrix is incomplete");
    require(wordLengths[0x1C] == 0x1C && wordLengths[0x1D] == 0x1D,
            "S7 timer/counter word length is incorrect");

    collector::ElementDefinition decoded;
    decoded.dataType = "LREAL";
    const std::array<std::uint8_t, 8> one{0x3F, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    require(collector::s7::detail::decodeJson(one, decoded) == "1", "S7 LREAL was not decoded");
    decoded.dataType = "STRING";
    const std::array<std::uint8_t, 4> text{'A', 'B', 'C', 0x00};
    require(collector::s7::detail::decodeJson(text, decoded) == "\"ABC\"", "S7 STRING was not decoded");

    collector::RuntimeSnapshot serverSnapshot;
    serverSnapshot.links.push_back({.id = "s7-server",
                                    .name = "S7 Server",
                                    .mode = "TCP Server",
                                    .protocol = "S7",
                                    .status = "enabled"});
    auto serverDevice = device;
    serverDevice.id = "s7-server-device";
    serverDevice.code = "860406088591522";
    serverDevice.linkId = "s7-server";
    serverDevice.linkMode = "TCP Server";
    serverDevice.targetId.clear();
    serverDevice.registrationMode = "HEX";
    serverDevice.registrationBytes = {0x08, 0x60, 0x40, 0x60, 0x88, 0x59, 0x15, 0x22};
    serverSnapshot.devices.push_back(serverDevice);
    auto conflictingDevice = serverDevice;
    conflictingDevice.id = "s7-server-device-2";
    conflictingDevice.code = "860406088591523";
    conflictingDevice.registrationBytes.back() = 0x23;
    serverSnapshot.devices.push_back(conflictingDevice);

    collector::ProtocolEngine serverEngine(runtimes());
    serverEngine.reload(serverSnapshot);
    (void)serverEngine.connected({.connectionId = "s7-server-connection",
                                  .linkId = "s7-server",
                                  .sessionEpoch = 1});
    service::message::IngressPacket serverPacket{.messageId = "s7-registration",
                                                .linkId = "s7-server",
                                                .connectionId = "s7-server-connection",
                                                .occurredAtMs = 4000,
                                                .payload = serverDevice.registrationBytes};
    actions = serverEngine.consume(serverPacket);
    require(has(actions, collector::ProtocolActionKind::BindDevice),
            "S7 standalone registration packet did not bind the device");
    require(first(actions, collector::ProtocolActionKind::Send).bytes == expectedCotp,
            "S7 registration did not start the PLC session");
    const auto handshakeDeadline = std::find_if(
        actions.begin(), actions.end(), [](const auto& action) {
            return action.kind == collector::ProtocolActionKind::ScheduleDeadline &&
                   action.deadlineAfter == std::chrono::seconds(5);
        });
    require(handshakeDeadline != actions.end(), "S7 registration omitted handshake timeout");
    const auto handshakeDeadlineToken = handshakeDeadline->deadlineToken;
    actions = serverEngine.consume(serverPacket);
    require(!has(actions, collector::ProtocolActionKind::Close),
            "S7 repeated registration packet closed its own DTU connection");
    serverPacket.payload = conflictingDevice.registrationBytes;
    actions = serverEngine.consume(serverPacket);
    require(first(actions, collector::ProtocolActionKind::Close).reason == "s7_registration_conflict",
            "S7 conflicting registration was not rejected");
    actions = serverEngine.deadline("s7-server-connection", handshakeDeadlineToken);
    require(!has(actions, collector::ProtocolActionKind::Close),
            "S7 handshake failure incorrectly closed the persistent DTU socket");

    (void)serverEngine.connected({.connectionId = "s7-prefixed-registration",
                                  .linkId = "s7-server",
                                  .sessionEpoch = 2});
    serverPacket.connectionId = "s7-prefixed-registration";
    serverPacket.payload = serverDevice.registrationBytes;
    const auto cotpConfirm = s7CotpConfirm();
    serverPacket.payload.insert(serverPacket.payload.end(), cotpConfirm.begin(), cotpConfirm.end());
    actions = serverEngine.consume(serverPacket);
    require(has(actions, collector::ProtocolActionKind::BindDevice),
            "S7 registration prefix did not bind the device");
    require(std::count_if(actions.begin(), actions.end(), [](const auto& action) {
                return action.kind == collector::ProtocolActionKind::Send;
            }) == 2,
            "S7 registration prefix payload was not preserved for protocol parsing");
}

void testS7AllDataTypes() {
    struct TypeCase {
        std::string_view type;
        std::vector<std::uint8_t> bytes;
        std::string_view expected;
        std::int64_t startBit = 0;
    };
    const std::array cases{
        TypeCase{"BOOL", {0x08}, "1", 3},
        TypeCase{"INT8", {0xFE}, "-2"},
        TypeCase{"UINT8", {0xFE}, "254"},
        TypeCase{"INT16", {0xFF, 0xFE}, "-2"},
        TypeCase{"UINT16", {0x12, 0x34}, "4660"},
        TypeCase{"INT32", {0xFF, 0xFF, 0xFF, 0xFE}, "-2"},
        TypeCase{"UINT32", {0x12, 0x34, 0x56, 0x78}, "305419896"},
        TypeCase{"FLOAT", {0x3F, 0xC0, 0x00, 0x00}, "1.5"},
        TypeCase{"LREAL", {0x3F, 0xF8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, "1.5"},
        TypeCase{"STRING", {'A', 'B', 'C', 0x00}, "\"ABC\""},
    };
    for (const auto& current : cases) {
        collector::ElementDefinition element;
        element.dataType = current.type;
        element.startBit = current.startBit;
        const auto decoded = collector::s7::detail::decodeJson(current.bytes, element);
        require(decoded && *decoded == current.expected, "S7 data type matrix failed");
    }
    const std::string invalidUtf8{static_cast<char>(0xC0), '"'};
    require(collector::s7::detail::jsonEscape(invalidUtf8) == "\\u00C0\\\"",
            "S7 invalid string bytes did not produce valid JSON escapes");
    require(collector::s7::detail::jsonEscape("中文") == "中文", "S7 valid UTF-8 text was not preserved");
}

void testWorkerTimer() {
    asio::io_context io;
    collector::Timer scheduler(io);
    int completed = 0;
    const auto cancelled =
        scheduler.scheduleAfter(std::chrono::milliseconds(1), [&] { completed += 100; });
    scheduler.cancel(cancelled);
    (void)scheduler.scheduleAfter(std::chrono::milliseconds(1), [&] { ++completed; });
    (void)scheduler.scheduleAfter(std::chrono::milliseconds(2), [&] {
        ++completed;
        scheduler.stop();
    });
    io.run();
    require(completed == 2, "worker timer cancellation or execution failed");
}

void testRuntimeWritableContract() {
    collector::RuntimeSnapshot readOnly;
    collector::DeviceDefinition device;
    device.id = "runtime-config-device";
    device.elements.push_back({.id = "runtime-config-element", .writable = false});
    readOnly.devices.push_back(device);

    auto writable = readOnly;
    writable.devices.front().elements.front().writable = true;
    require(service::collector::config::signature(readOnly) !=
                service::collector::config::signature(writable),
            "runtime signature ignored writable changes");

    const auto expectSignatureChange = [&readOnly](std::string_view field, auto mutate) {
        auto changed = readOnly;
        mutate(changed.devices.front(), changed.devices.front().elements.front());
        require(service::collector::config::signature(readOnly) !=
                    service::collector::config::signature(changed),
                std::string("runtime signature ignored ") + std::string(field));
    };
    expectSignatureChange("Modbus merge gap", [](auto& changed, auto&) {
        changed.modbusMergeGap += 1;
    });
    expectSignatureChange("Modbus maximum quantity", [](auto& changed, auto&) {
        changed.modbusMaxQuantity -= 1;
    });
    expectSignatureChange("storage interval", [](auto& changed, auto&) {
        changed.storageInterval += 1;
    });
    expectSignatureChange("command fast-read duration", [](auto& changed, auto&) {
        changed.commandFastReadDuration += 1;
    });
    expectSignatureChange("command fast-read interval", [](auto& changed, auto&) {
        changed.commandFastReadInterval += 1;
    });
    expectSignatureChange("element config key", [](auto&, auto& changed) {
        changed.configKey = "element:replacement";
    });

    const auto element = service::collector::config::detail::element(
        {{"id", "runtime-config-element"}, {"writable", "1"}});
    require(element.writable, "runtime did not deserialize writable state");
    bool rejectedInvalidScale = false;
    try {
        (void)service::collector::config::detail::element(
            {{"id", "runtime-config-element"}, {"scale", "1x"}});
    } catch (const std::runtime_error& error) {
        rejectedInvalidScale =
            std::string_view(error.what()).find("invalid runtime decimal: scale") !=
            std::string_view::npos;
    }
    require(rejectedInvalidScale,
            "runtime accepted an element scale with trailing non-decimal bytes");
    bool rejectedOutOfRangeSlave = false;
    try {
        (void)service::collector::config::detail::device(
            {{"id", "runtime-config-device"}, {"slave_id", "300"}});
    } catch (const std::runtime_error& error) {
        rejectedOutOfRangeSlave =
            std::string_view(error.what()).find("invalid runtime integer range: slave_id") !=
            std::string_view::npos;
    }
    require(rejectedOutOfRangeSlave,
            "runtime wrapped an out-of-range Modbus slave id");
    bool rejectedInvalidWritable = false;
    try {
        (void)service::collector::config::detail::element(
            {{"id", "runtime-config-element"}, {"writable", "2"}});
    } catch (const std::runtime_error& error) {
        rejectedInvalidWritable =
            std::string_view(error.what()).find("invalid runtime boolean: writable") !=
            std::string_view::npos;
    }
    require(rejectedInvalidWritable,
            "runtime accepted a non-boolean writable flag");
    bool rejectedInvalidHeartbeat = false;
    try {
        (void)service::collector::config::detail::device(
            {{"id", "runtime-config-device"}, {"heartbeat_hex", "0G"}});
    } catch (const std::runtime_error& error) {
        rejectedInvalidHeartbeat =
            std::string_view(error.what()).find("invalid runtime hex: heartbeat_hex") !=
            std::string_view::npos;
    }
    require(rejectedInvalidHeartbeat,
            "runtime silently dropped an invalid heartbeat HEX payload");
}

void testRuntimeRepositoryRejectsInvalidScale() {
    RuntimeRepositoryScaleDb db;
    bool rejected = false;
    try {
        (void)runTask(service::runtime::repository::loadRuntimeSnapshot(db));
    } catch (const std::exception& error) {
        rejected = std::string_view(error.what()).find(
                       "invalid runtime repository decimal: scale") != std::string_view::npos;
    }
    require(rejected, "runtime repository accepted a Modbus scale with trailing bytes");
}

void testAtomicStreamFinalizationContract() {
    const std::vector<service::message::StreamField> deadLetterFields{
        {"source_entry_id", "10-0"}};
    const std::vector<service::message::StreamField> resultFields{
        {"command_id", "command-1"}};
    const std::array publications{
        service::message::redis::StreamPublication{"dead-letter", deadLetterFields, 100},
        service::message::redis::StreamPublication{"command-result", resultFields, 200}};

    RecordingRedis committed(1);
    require(runTask(service::message::redis::publishAllAndAcknowledge(
                committed, publications, "command-input", "collector-group", "collector-0",
                "10-0")),
            "pending stream finalization did not report a commit");
    require(committed.keys ==
                std::vector<std::string>{"dead-letter", "command-result", "command-input"},
            "atomic stream finalization changed Redis key order");
    require(committed.arguments ==
                std::vector<std::string>{"collector-group", "10-0", "collector-0", "100",
                                         "1", "source_entry_id", "10-0", "200", "1",
                                         "command_id", "command-1"},
            "atomic stream finalization encoded invalid arguments");
    const auto pendingCheck = committed.script.find("XPENDING");
    const auto typeCheck = committed.script.find("TYPE");
    const auto firstWrite = committed.script.find("XADD");
    require(pendingCheck != std::string::npos && typeCheck != std::string::npos &&
                firstWrite != std::string::npos && pendingCheck < firstWrite &&
                typeCheck < firstWrite,
            "atomic stream finalization writes before validating ownership and key types");
    require(committed.script.find("pending[1][2] ~= ARGV[3]") != std::string::npos,
            "atomic stream finalization did not enforce consumer ownership");

    RecordingRedis missingPending(0);
    require(!runTask(service::message::redis::publishAllAndAcknowledge(
                missingPending, publications, "command-input", "collector-group", "collector-0",
                "10-0")),
            "missing PEL entry was reported as atomically finalized");
}

void testRuntimeProjectionRejectsRedisErrors() {
    FailingConfigRedis redis;
    bool rejected = false;
    try {
        (void)runTask(service::collector::config::project(
            redis, service::collector::RuntimeSnapshot{}));
    } catch (const std::runtime_error& error) {
        rejected = std::string_view(error.what()).find("active runtime") !=
                   std::string_view::npos;
    }
    require(rejected,
            "runtime projection reported success after the active-version SET failed");
}

void testRuntimeProjectionRefreshesPreviousGrace() {
    FailingConfigRedis redis(false, true);
    (void)runTask(service::collector::config::project(
        redis, service::collector::RuntimeSnapshot{}));
    const auto refresh = std::ranges::find_if(redis.commands, [](const auto& command) {
        return command.size() >= 4 && command.front() == "ZADD" &&
               command.back() == "previous-version";
    });
    require(refresh != redis.commands.end(),
            "runtime projection did not retain the previous snapshot");
    require(refresh->size() == 4 && (*refresh)[2] != "NX",
            "runtime projection did not restart the previous snapshot grace window");
}

void testRuntimeSetOrderingContract() {
    const auto first = service::collector::config::detail::sortedStrings(
        {"element:c", "element:a", "element:b"});
    const auto second = service::collector::config::detail::sortedStrings(
        {"element:b", "element:c", "element:a"});
    require(first == second &&
                first == std::vector<std::string>{"element:a", "element:b", "element:c"},
            "runtime Redis Set members were not canonicalized");

    collector::RuntimeSnapshot previous;
    collector::LinkDefinition link;
    link.id = "s7-server";
    link.mode = "TCP Server";
    link.protocol = "S7";
    previous.links.push_back(link);

    collector::DeviceDefinition device;
    device.id = "s7-device";
    device.linkId = link.id;
    device.linkMode = link.mode;
    device.protocol = link.protocol;
    for (const auto& id : first)
        device.elements.push_back({.configKey = id, .id = id});
    previous.devices.push_back(device);

    auto next = previous;
    next.devices.front().elements.clear();
    for (const auto& id : second)
        next.devices.front().elements.push_back({.configKey = id, .id = id});
    const auto plan = collector::planRuntimeReconcile(previous, next);
    require(plan.affectedLinks.empty() && plan.restartLinks.empty(),
            "equivalent Redis Set order restarted an unrelated TCP Server link");
}

void testRealtimeProjectionContract() {
    collector::RuntimeSnapshot original;
    collector::RealtimeDeviceDefinition device;
    device.id = "019fd9f6-4bd5-7ec3-80ff-0ba381987024";
    device.code = "REMOTEIO01";
    device.name = "远程IO";
    device.points.push_back({"AI1", "模拟量输入1", "mA"});
    original.realtimeDevices.push_back(device);

    const auto packed = service::collector::config::detail::encodeRealtimePoint(
        original.realtimeDevices.front().points.front());
    const auto decoded = service::collector::config::detail::decodeRealtimePoint(packed);
    require(decoded.has_value() && decoded->id == "AI1" &&
                decoded->name == "模拟量输入1" && decoded->unit == "mA",
            "realtime point projection encoding changed");
    require(!service::collector::config::detail::decodeRealtimePoint("broken").has_value(),
            "malformed realtime point projection was accepted");
    require(service::collector::config::realtimeDeviceKey("version-1", device.id) ==
                "iot:config:runtime:version-1:realtime-device:" + device.id,
            "realtime device projection key changed");

    auto renamed = original;
    renamed.realtimeDevices.front().name = "远程IO-更新";
    require(service::collector::config::signature(original) !=
                service::collector::config::signature(renamed),
            "runtime signature ignored realtime metadata changes");
}

void testFreshnessDeadlineWait() {
    namespace latest = service::telemetry::latest;
    require(!latest::deadlineWait(1000, std::nullopt).has_value(),
            "missing deadline must block until a wake event");
    require(latest::deadlineWait(1000, 999) == std::chrono::milliseconds::zero(),
            "past deadline must run immediately");
    require(latest::deadlineWait(1000, 1250) == std::chrono::milliseconds(250),
            "future deadline wait changed");
}

void testEdgeSessionOwnership() {
    EdgeSessionRedis redis;
    constexpr std::string_view nodeId = "00000000-0000-7000-8000-000000000002";
    require(runTask(service::edge::session_state::claim(redis, nodeId, 11)),
            "initial edge session claim failed");
    require(runTask(service::edge::session_state::claim(redis, nodeId, 22)),
            "replacement edge session claim failed");
    require(!runTask(service::edge::session_state::refresh(redis, nodeId, 11)),
            "stale edge session retained ownership");
    require(redis.value == "22", "stale edge session overwrote the replacement epoch");
    require(!runTask(service::edge::session_state::release(redis, nodeId, 11)),
            "stale edge session reported replacement cleanup");
    require(redis.value == "22", "stale edge session deleted the replacement epoch");
    require(runTask(service::edge::session_state::refresh(redis, nodeId, 22)),
            "active edge session failed to refresh ownership");
    require(runTask(service::edge::session_state::release(redis, nodeId, 22)) && !redis.value,
            "active edge session failed to release ownership");
}

void testLatestProjectionRejectsRedisErrors() {
    bool rejected = false;
    try {
        runTask(service::telemetry::latest::executeProjectionPipeline(
            FailingLatestPipeline{}, "test latest projection"));
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, "latest-value projection ignored a Redis pipeline error");
}

void testLatestProjectionRefreshesPreservedElementMetadata() {
    LatestProjectionContext context;
    runTask(service::telemetry::latest::projectDevice(context, "device-1"));

    bool skippedExistingElement = false;
    bool refreshesElementMetadata = false;
    for (const auto& command : context.redisClient.state->pipelineCommands) {
        if (command.size() >= 4 && command[0] == "HSETNX" &&
            command[1] == "iot:device:D1:latest" && command[2] == "temperature")
            skippedExistingElement = true;
        if (!command.empty() && command[0] != "HSETNX") {
            const auto hasElement = std::ranges::find(command, "temperature") != command.end();
            const auto hasUpdatedName = std::ranges::any_of(command, [](const auto& argument) {
                return argument.find("\"name\":\"New Name\"") != std::string::npos;
            });
            refreshesElementMetadata = refreshesElementMetadata || (hasElement && hasUpdatedName);
        }
    }
    require(!skippedExistingElement,
            "latest projection skipped existing element metadata refresh with HSETNX");
    require(refreshesElementMetadata,
            "latest projection did not write refreshed metadata for an existing element");
}

void testLatestProjectionRejectsInvalidPreservedDeadline() {
    LatestProjectionContext context;
    context.redisClient.state->preservedOnlineUntilMs = "not-a-number";
    runTask(service::telemetry::latest::projectDevice(context, "device-1"));

    bool emittedInvalidDeadline = false;
    bool clearedDeadline = false;
    for (const auto& command : context.redisClient.state->pipelineCommands) {
        if (command.size() >= 4 && command[0] == "ZADD" &&
            command[1] == "iot:schedule:device:online-deadlines" &&
            command[2] == "not-a-number")
            emittedInvalidDeadline = true;
        if (command.size() >= 3 && command[0] == "ZREM" &&
            command[1] == "iot:schedule:device:online-deadlines" && command[2] == "D1")
            clearedDeadline = true;
    }
    require(!emittedInvalidDeadline,
            "latest projection reused an invalid preserved online deadline");
    require(clearedDeadline, "latest projection did not clear an invalid preserved deadline");
}

void testAlertScheduleSkipsInvalidStoredDuration() {
    AlertScheduleRedis redis;
    service::message::ParsedDeviceMessage message;
    message.deviceId = "device-1";
    message.deviceCode = "D1";
    message.observedAtMs = 1700000000000;

    runTask(service::alert::metadata::schedule(redis, std::vector{message}));
    require(!redis.state->pipelineCommands.empty(),
            "alert schedule did not issue a Redis pipeline command");
}

void testGroupedBoundedStreamSkipsInvalidDepth() {
    GroupedBoundedRedis redis;
    const auto id = runTask(service::message::redis::addGroupedBounded(
        redis, "iot:test:stream",
        std::vector<service::message::StreamField>{{"field", "value"}},
        100, "iot:test:depth", 10));
    require(id.has_value() && *id == "1-0",
            "grouped bounded stream rejected a write when depth key was nonnumeric");
}

void testGroupedAckDoesNotDecrementStaleDepth() {
    GroupedAckRedis redis;
    runTask(service::message::redis::acknowledgeGroupedAndDelete(
        redis, "iot:test:stream", "iot-engine:test", "stale-id", "iot:test:depth"));
    require(redis.depth == 5,
            "grouped stream stale ACK decremented the queued-depth counter");
}

void testCommandValueDecimalParsing() {
    namespace command = service::collector::command;
    require(command::decimal("1.5", "value") == 1.5,
            "command decimal parser rejected a finite value");
    require(command::decimal("-1.25e2", "value") == -125.0,
            "command decimal parser rejected scientific notation");

    for (const auto value : {"1.5x", "nan", "inf"}) {
        bool rejected = false;
        try {
            (void)command::decimal(value, "value");
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "command decimal parser accepted an invalid value");
    }

    collector::ElementDefinition bcd;
    bcd.id = "bcd-value";
    bcd.name = "BCD value";
    bcd.encoding = "BCD";
    bcd.length = 2;
    bcd.digits = 2;
    bool rejectedNegativeBcd = false;
    try {
        command::validateValue(bcd, "-12.34");
    } catch (const std::invalid_argument&) {
        rejectedNegativeBcd = true;
    }
    require(rejectedNegativeBcd,
            "command validation accepted a negative BCD value that encodes as positive");

    bool rejectedNegativeBcdEncoding = false;
    try {
        (void)collector::sl651::detail::encodeValue(bcd, "-12.34");
    } catch (const std::invalid_argument&) {
        rejectedNegativeBcdEncoding = true;
    }
    require(rejectedNegativeBcdEncoding,
            "SL651 encoded a negative BCD value as a positive wire value");
}

void testPacketLog() {
    namespace packetLog = service::common::packet_log;
    const auto directory = std::filesystem::temp_directory_path() / "iot-engine-packet-log-test";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);

    packetLog::Config config;
    config.directory = directory;
    config.level = packetLog::Level::Debug;
    packetLog::initialize(std::move(config));

    packetLog::Context context;
    context.workerIndex = 2;
    context.direction = "RX";
    context.operation = "transport";
    context.protocol = "Modbus";
    context.linkId = "link-test";
    context.connectionId = "connection-test";
    context.messageId = "message-test";
    context.sessionEpoch = 7;
    const std::array<std::uint8_t, 4> invalidBytes{0x00, 0xFF, 0x7E, 0x01};
    packetLog::write(packetLog::Level::Debug, "RX_BYTES", context, invalidBytes);
    packetLog::write(packetLog::Level::Warn, "TIMEOUT", context, {}, "modbus_response_timeout");
    packetLog::write(packetLog::Level::Info, "BROADCAST_START", context, {}, "targets=2");
    packetLog::shutdown();

    std::string content;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file())
            continue;
        std::ifstream input(entry.path());
        content.append(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }
    require(content.find("event=\"RX_BYTES\"") != std::string::npos &&
                content.find("hex=\"00 FF 7E 01\"") != std::string::npos,
            "packet log did not preserve unparsed binary bytes");
    require(content.find("event=\"TIMEOUT\"") != std::string::npos &&
                content.find("event=\"BROADCAST_START\"") != std::string::npos,
            "packet log did not persist timeout and broadcast events");
    std::filesystem::remove_all(directory, ignored);
}

collector::RuntimeSnapshot clientReconcileSnapshot() {
    collector::RuntimeSnapshot snapshot;
    collector::LinkDefinition link{.id = "client-link",
                                   .name = "Client",
                                   .mode = "TCP Client",
                                   .protocol = "Modbus",
                                   .status = "enabled"};
    link.targets.push_back(
        {.id = "target-a", .name = "A", .ip = "127.0.0.1", .port = 15001,
         .status = "enabled"});
    link.targets.push_back(
        {.id = "target-b", .name = "B", .ip = "127.0.0.1", .port = 15002,
         .status = "enabled"});
    snapshot.links.push_back(link);

    collector::DeviceDefinition first;
    first.id = "device-a";
    first.code = "A";
    first.linkId = link.id;
    first.linkMode = link.mode;
    first.targetId = "target-a";
    first.protocol = link.protocol;
    first.modbusMode = "TCP";
    first.slaveId = 1;
    snapshot.devices.push_back(first);

    auto second = first;
    second.id = "device-b";
    second.code = "B";
    second.targetId = "target-b";
    snapshot.devices.push_back(second);
    return snapshot;
}

void testRuntimeReconcile() {
    const auto previous = clientReconcileSnapshot();

    auto deviceUpdate = previous;
    deviceUpdate.devices.front().pollInterval = 10;
    const auto devicePlan = collector::planRuntimeReconcile(previous, deviceUpdate);
    require(devicePlan.affectedLinks.contains("client-link"),
            "device update did not affect its link");
    require(devicePlan.refreshClientSessions.contains({"client-link", "target-a"}),
            "changed Modbus device target was not scheduled for a session refresh");
    require(!devicePlan.refreshClientSessions.contains({"client-link", "target-b"}),
            "unchanged sibling target was scheduled for a session refresh");
    require(devicePlan.restartClientTargets.empty(),
            "Modbus device update incorrectly restarted its TCP Client transport");
    require(devicePlan.restartLinks.empty(),
            "TCP Client device update incorrectly restarted the whole link");

    auto metadataUpdate = previous;
    metadataUpdate.links.front().name = "Renamed client";
    metadataUpdate.links.front().targets.back().name = "Renamed B";
    const auto metadataPlan = collector::planRuntimeReconcile(previous, metadataUpdate);
    require(metadataPlan.affectedLinks.contains("client-link"),
            "link metadata update was not observed");
    require(metadataPlan.refreshClientSessions.empty() &&
                metadataPlan.restartClientTargets.empty() && metadataPlan.restartLinks.empty(),
            "metadata-only update restarted a TCP Client transport");

    auto s7Previous = previous;
    s7Previous.links.front().protocol = "S7";
    for (auto& device : s7Previous.devices)
        device.protocol = "S7";
    auto s7Next = s7Previous;
    s7Next.devices.front().pollInterval = 10;
    const auto s7Plan = collector::planRuntimeReconcile(s7Previous, s7Next);
    require(s7Plan.restartClientTargets.contains({"client-link", "target-a"}) &&
                s7Plan.refreshClientSessions.empty(),
            "stateful S7 session update did not retain transport restart semantics");

    auto rtuPrevious = previous;
    rtuPrevious.devices.front().modbusMode = "RTU";
    auto rtuNext = rtuPrevious;
    rtuNext.devices.front().pollInterval = 10;
    const auto rtuPlan = collector::planRuntimeReconcile(rtuPrevious, rtuNext);
    require(rtuPlan.restartClientTargets.contains({"client-link", "target-a"}) &&
                rtuPlan.refreshClientSessions.empty(),
            "Modbus RTU-over-TCP update did not retain transport restart semantics");

    auto serverPrevious = previous;
    serverPrevious.links.front().mode = "TCP Server";
    serverPrevious.links.front().targets.clear();
    serverPrevious.devices.front().linkMode = "TCP Server";
    serverPrevious.devices.back().linkMode = "TCP Server";
    auto serverNext = serverPrevious;
    serverNext.devices.front().pollInterval = 10;
    const auto serverPlan = collector::planRuntimeReconcile(serverPrevious, serverNext);
    require(serverPlan.restartLinks.contains("client-link"),
            "TCP Server device update did not restart the server link");

    collector::ProtocolEngine engine(runtimes());
    engine.reload(previous);
    (void)engine.connected({.connectionId = "preserved-connection",
                            .linkId = "client-link",
                            .remoteAddress = "127.0.0.1:15002",
                            .targetId = "target-b",
                            .sessionEpoch = 1});
    engine.reload(metadataUpdate, metadataPlan.affectedLinks);
    require(engine.contains("preserved-connection"),
            "metadata reload discarded a preserved TCP Client session");

    auto pollingPrevious = previous;
    pollingPrevious.devices.front().elements.push_back(
        {.id = "holding-0",
         .name = "Holding 0",
         .dataType = "UINT16",
         .registerType = "HOLDING_REGISTER",
         .quantity = 1});
    auto pollingUpdate = pollingPrevious;
    pollingUpdate.devices.front().pollInterval = 10;
    const auto pollingPlan =
        collector::planRuntimeReconcile(pollingPrevious, pollingUpdate);

    collector::ProtocolEngine refreshEngine(runtimes());
    refreshEngine.reload(pollingPrevious);
    const auto initialActions =
        refreshEngine.connected({.connectionId = "refreshed-connection",
                                 .linkId = "client-link",
                                 .remoteAddress = "127.0.0.1:15001",
                                 .targetId = "target-a",
                                 .sessionEpoch = 2});
    require(has(initialActions, collector::ProtocolActionKind::BindDevice) &&
                has(initialActions, collector::ProtocolActionKind::Send),
            "initial Modbus client session was not active");
    refreshEngine.reload(pollingUpdate, pollingPlan.affectedLinks);
    const auto refreshes =
        refreshEngine.refreshClientSessions(pollingPlan.refreshClientSessions);
    require(refreshes.size() == 1 &&
                refreshes.front().connectionId == "refreshed-connection",
            "changed Modbus target did not rebuild its protocol session");
    require(has(refreshes.front().retiredActions,
                collector::ProtocolActionKind::CancelDeadline) &&
                has(refreshes.front().startedActions,
                    collector::ProtocolActionKind::BindDevice) &&
                has(refreshes.front().startedActions, collector::ProtocolActionKind::Send),
            "protocol session refresh did not retire old work and start the new definition");
    const auto& initialSend = first(initialActions, collector::ProtocolActionKind::Send);
    const auto& refreshedSend =
        first(refreshes.front().startedActions, collector::ProtocolActionKind::Send);
    require(initialSend.bytes.size() >= 2 && refreshedSend.bytes.size() >= 2 &&
                (initialSend.bytes[0] != refreshedSend.bytes[0] ||
                 initialSend.bytes[1] != refreshedSend.bytes[1]),
            "refreshed Modbus TCP session reused an in-flight transaction id");
    require(refreshEngine.contains("refreshed-connection"),
            "protocol session refresh discarded the live TCP connection identity");
}

void testPollStagger() {
    const auto now = std::chrono::steady_clock::time_point(std::chrono::seconds(12345));
    const auto interval = std::chrono::seconds(997);
    const auto delay = collector::staggeredPollDelay("device-a", interval, now);
    require(delay > std::chrono::milliseconds::zero() && delay <= interval,
            "staggered poll delay escaped its configured interval");
    require(delay == collector::staggeredPollDelay("device-a", interval, now),
            "poll phase was not stable for the same device");
    const auto nextEpoch =
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch() + delay).count();
    require(nextEpoch % interval.count() ==
                collector::stablePollHash("device-a") %
                    static_cast<std::uint64_t>(interval.count()),
            "poll deadline did not land on the device's stable phase");

    std::set<std::chrono::milliseconds> phases;
    for (int index = 0; index < 32; ++index)
        phases.insert(collector::staggeredPollDelay(
            "device-" + std::to_string(index), interval, now));
    require(phases.size() > 1, "different devices collapsed onto one polling phase");
    require(collector::staggeredPollDelay("device-a", std::chrono::seconds(1), now) ==
                std::chrono::seconds(1),
            "one-second polling interval was changed by staggering");
}

void testTcpClientTargetReconcile() {
    asio::io_context io;
    collector::Timer scheduler(io);
    asio::ip::tcp::acceptor firstServer(
        io, {asio::ip::make_address("127.0.0.1"), 0});
    asio::ip::tcp::acceptor secondServer(
        io, {asio::ip::make_address("127.0.0.1"), 0});
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> firstAccepted;
    std::vector<std::shared_ptr<asio::ip::tcp::socket>> secondAccepted;
    const auto acceptOne = [&io](
                               asio::ip::tcp::acceptor& server,
                               std::vector<std::shared_ptr<asio::ip::tcp::socket>>& accepted) {
        auto socket = std::make_shared<asio::ip::tcp::socket>(io);
        server.async_accept(*socket, [socket, &accepted](const std::error_code& error) {
            if (!error)
                accepted.push_back(socket);
        });
    };
    acceptOne(firstServer, firstAccepted);
    acceptOne(secondServer, secondAccepted);

    std::map<std::string, std::vector<std::string>, std::less<>> connectedByTarget;
    std::vector<std::string> disconnected;
    collector::Tcp tcp(
        io, scheduler, 0, 1,
        [&connectedByTarget](collector::ProtocolConnectionInfo info) {
            connectedByTarget[info.targetId].push_back(std::move(info.connectionId));
        },
        [](service::message::IngressPacket) {},
        [&disconnected](std::string connectionId, std::string) {
            disconnected.push_back(std::move(connectionId));
        },
        [](collector::LinkState) {}, false,
        [](std::string, collector::Tcp::NativeSocket handle, std::string) {
            collector::Tcp::closeNative(handle);
        });

    auto previous = clientReconcileSnapshot();
    previous.links.front().targets.front().port = firstServer.local_endpoint().port();
    previous.links.front().targets.back().port = secondServer.local_endpoint().port();
    tcp.reload(previous);
    io.run_for(std::chrono::milliseconds(250));
    require(connectedByTarget["target-a"].size() == 1 &&
                connectedByTarget["target-b"].size() == 1,
            "initial TCP Client targets did not both connect");
    const auto originalFirst = connectedByTarget["target-a"].front();
    const auto originalSecond = connectedByTarget["target-b"].front();

    auto next = previous;
    next.devices.front().pollInterval = 10;
    const auto plan = collector::planRuntimeReconcile(previous, next);
    tcp.reconcile(next, plan);
    io.restart();
    io.run_for(std::chrono::milliseconds(250));

    require(connectedByTarget["target-a"].size() == 1 &&
                connectedByTarget["target-a"].front() == originalFirst,
            "Modbus device update reconnected its unchanged TCP Client target");
    require(connectedByTarget["target-b"].size() == 1 &&
                connectedByTarget["target-b"].front() == originalSecond,
            "unchanged sibling target was disconnected");
    require(std::ranges::find(disconnected, originalFirst) == disconnected.end() &&
                std::ranges::find(disconnected, originalSecond) == disconnected.end(),
            "device-only update emitted a TCP disconnect notification");
    require(tcp.advanceSessionEpoch(originalFirst) != 0,
            "live TCP connection could not rotate its protocol session epoch");

    tcp.stop();
    firstServer.close();
    secondServer.close();
    scheduler.stop();
    io.stop();
}

void testTcpCloseDuringPendingWrite() {
    asio::io_context io;
    collector::Timer scheduler(io);
    asio::ip::tcp::acceptor server(io, {asio::ip::make_address("127.0.0.1"), 0});
    auto accepted = std::make_shared<asio::ip::tcp::socket>(io);
    server.async_accept(*accepted, [](const std::error_code&) {});

    std::string connectionId;
    int writeCompletions = 0;
    collector::Tcp tcp(
        io, scheduler, 0, 1,
        [&connectionId](collector::ProtocolConnectionInfo info) {
            connectionId = std::move(info.connectionId);
        },
        [](service::message::IngressPacket) {}, [](std::string, std::string) {},
        [](collector::LinkState) {}, false,
        [](std::string, collector::Tcp::NativeSocket handle, std::string) {
            collector::Tcp::closeNative(handle);
        });

    auto snapshot = clientReconcileSnapshot();
    snapshot.links.front().targets.resize(1);
    snapshot.links.front().targets.front().port = server.local_endpoint().port();
    snapshot.devices.resize(1);
    snapshot.devices.front().targetId = snapshot.links.front().targets.front().id;
    tcp.reload(snapshot);
    io.run_for(std::chrono::milliseconds(250));
    require(!connectionId.empty(), "pending-write fixture did not connect");

    io.restart();
    tcp.send(connectionId, std::vector<std::uint8_t>(8 * 1024 * 1024, 0x5A),
             [&writeCompletions](bool) { ++writeCompletions; });
    tcp.close(connectionId, "test_close_during_write");
    io.run_for(std::chrono::milliseconds(250));
    require(writeCompletions == 1,
            "closing a pending TCP write completed its callback more than once");

    tcp.stop();
    server.close();
    accepted->close();
    scheduler.stop();
    io.stop();
}

void testEdgeParsedMessageContract() {
    service::message::ParsedDeviceMessage parsed;
    parsed.messageId = "019f91c9-4087-7e6c-88c0-c431b0dc15d8";
    parsed.causationId = parsed.messageId;
    parsed.linkId = "019f91bf-6f83-7491-8a53-cd4fde034b73";
    parsed.deviceId = "019f91bf-6f83-7491-8a53-cd4fde034b72";
    parsed.deviceCode = "PCS7";
    parsed.protocol = "S7";
    parsed.connectionId = "019f8c99-913c-7a0c-b6ad-c43bb9b12764";
    parsed.occurredAtMs = 1784856700000;
    parsed.observedAtMs = 1784856700000;
    parsed.source = "edge";
    parsed.valuesJson = R"json({"values":{"VW0":{"name":"VW0","value":1,"unit":""}}})json";

    service::message::StreamMessage streamMessage{.id = "1-0",
                                                  .fields = service::message::parsedFields(parsed)};
    const auto roundTrip = service::message::parsedFrom(streamMessage);
    require(roundTrip.linkId == parsed.linkId, "edge parsed message lost its link identity");
    require(roundTrip.rawPayloads.empty(), "edge parsed message changed empty raw payloads");
    require(roundTrip.deviceCode == "PCS7" && roundTrip.source == "edge",
            "edge parsed message did not round-trip required fields");

    parsed.occurredAtMs = 1784856700000;
    parsed.observedAtMs = 4102444800000;
    streamMessage.fields = service::message::parsedFields(parsed);
    const auto future = service::message::parsedFrom(streamMessage);
    require(future.observedAtMs == parsed.occurredAtMs,
            "future device timestamp poisoned telemetry ordering");

    parsed.observedAtMs = parsed.occurredAtMs - 60'000;
    streamMessage.fields = service::message::parsedFields(parsed);
    const auto historical = service::message::parsedFrom(streamMessage);
    require(historical.observedAtMs == parsed.observedAtMs,
            "valid historical device timestamp was replaced by receive time");
}

void testAtomicPendingCommandDispatch() {
    service::message::ProtocolTask first;
    first.messageId = "019f91c9-4087-7e6c-88c0-c431b0dc15d8";
    first.deviceId = "019f91bf-6f83-7491-8a53-cd4fde034b72";
    first.deviceCode = "D1";
    first.protocol = "Modbus";
    first.createdAtMs = 1000;
    auto second = first;
    second.messageId = "019f91c9-4087-7e6c-88c0-c431b0dc15d9";

    std::vector<service::command::PendingDispatch> dispatches;
    dispatches.push_back({.task = first,
                          .streamFields = {{"message_id", first.messageId},
                                           {"device_id", first.deviceId}}});
    dispatches.push_back({.task = second,
                          .streamFields = {{"message_id", second.messageId},
                                           {"device_id", second.deviceId}}});

    RecordingRedis redis(2);
    runTask(service::command::dispatchPendingBatch(
        redis, "iot:channel:command:worker:0:high",
        service::command::PendingQueueKind::Stream, dispatches, "user-1", 10'000));
    require(redis.keys.size() == 3 &&
                redis.keys[1] == "iot:state:command:" + first.messageId &&
                redis.keys[2] == "iot:state:command:" + second.messageId,
            "pending command batch did not bind every state key to one Redis script");
    require(redis.script.find("redis.call('XADD'") < redis.script.find("redis.call('HSET'"),
            "pending state was written before the command queue entry");
}

} // namespace

int main() {
    try {
        const auto run = [](std::string_view name, auto&& test) {
            std::cerr << "[ RUN      ] " << name << '\n';
            test();
        };
        run("capabilities", testCapabilities);
        run("runtime reconcile", testRuntimeReconcile);
        run("poll stagger", testPollStagger);
        run("TCP Client target reconcile", testTcpClientTargetReconcile);
        run("TCP close during pending write", testTcpCloseDuringPendingWrite);
        run("sl651", testSl651);
        run("sl651 encodings", testSl651AllEncodingsAndFunctionCodes);
        run("sl651 multi-packet images", testSl651MultiPacketImages);
        run("modbus", testModbus);
        run("modbus types and priority", testModbusTypesAndPriority);
        run("modbus data types", testModbusAllDataTypesAndByteOrders);
        run("modbus TCP functions", [] { testModbusAllFunctionCodes(true); });
        run("modbus RTU functions", [] { testModbusAllFunctionCodes(false); });
        run("modbus RTU zero address", testModbusRtuZeroAddress);
        run("modbus discovery and registration", testModbusDiscoveryAndOffline);
        run("s7", testS7);
        run("s7 data types", testS7AllDataTypes);
        run("worker timer", testWorkerTimer);
        run("runtime writable contract", testRuntimeWritableContract);
        run("runtime repository invalid scale", testRuntimeRepositoryRejectsInvalidScale);
        run("atomic stream finalization contract", testAtomicStreamFinalizationContract);
        run("runtime projection Redis errors", testRuntimeProjectionRejectsRedisErrors);
        run("runtime projection previous grace", testRuntimeProjectionRefreshesPreviousGrace);
        run("runtime set ordering contract", testRuntimeSetOrderingContract);
        run("realtime projection contract", testRealtimeProjectionContract);
        run("freshness deadline wait", testFreshnessDeadlineWait);
        run("edge session ownership", testEdgeSessionOwnership);
        run("latest projection Redis errors", testLatestProjectionRejectsRedisErrors);
        run("latest projection metadata refresh",
            testLatestProjectionRefreshesPreservedElementMetadata);
        run("latest projection invalid preserved deadline",
            testLatestProjectionRejectsInvalidPreservedDeadline);
        run("alert schedule invalid stored duration",
            testAlertScheduleSkipsInvalidStoredDuration);
        run("grouped bounded stream invalid depth",
            testGroupedBoundedStreamSkipsInvalidDepth);
        run("grouped stream stale ACK depth",
            testGroupedAckDoesNotDecrementStaleDepth);
        run("command value decimal parsing", testCommandValueDecimalParsing);
        run("edge parsed message contract", testEdgeParsedMessageContract);
        run("atomic pending command dispatch", testAtomicPendingCommandDispatch);
        run("packet log", testPacketLog);
        std::cout << "collector protocol tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "collector protocol test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
