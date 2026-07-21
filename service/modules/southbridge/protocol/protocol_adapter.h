#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "service/common/bridge/message.contract.h"
#include "service/modules/southbridge/runtime.types.h"

namespace service::southbridge {

class ProtocolAdapter {
  public:
    virtual ~ProtocolAdapter() = default;

    [[nodiscard]] virtual std::string_view protocol() const noexcept = 0;

    [[nodiscard]] virtual std::optional<std::string>
    identify(const bridge::IngressPacket& packet,
             const std::vector<DtuDefinition>& candidates) const = 0;

    [[nodiscard]] virtual std::vector<bridge::ParsedDeviceMessage>
    parse(const bridge::IngressPacket& packet, const DtuDefinition& dtu) const = 0;
};

} // namespace service::southbridge
