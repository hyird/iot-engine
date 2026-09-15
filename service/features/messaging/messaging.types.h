#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include "service/common/message.h"

namespace service::message::redis {

struct StreamWake final {
    std::optional<std::size_t> workerIndex{};
    WorkerStreamTask task{};
};

struct StreamPublication {
    std::string_view stream;
    std::span<const StreamField> fields;
    std::size_t maxLength = 0;
    std::optional<StreamWake> wake;
};

} // namespace service::message::redis
