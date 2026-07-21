#pragma once

#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "service/common/bridge/message.contract.h"
#include "service/modules/southbridge/runtime.types.h"

namespace service::southbridge {

enum class RegistrationMatchKind { None, StandaloneFrame, PrefixedPayload, Conflict };

struct RegistrationMatch {
    RegistrationMatchKind kind = RegistrationMatchKind::None;
    DtuDefinition dtu;
    std::vector<std::uint8_t> payload;
};

class DtuRegistry {
  public:
    void reload(const RuntimeSnapshot& snapshot) {
        std::map<std::string, DtuDefinition> definitions;
        std::map<std::string, std::vector<std::string>> byLink;
        for (const auto& device : snapshot.devices) {
            const auto key = makeKey(device);
            const auto registration = device.linkMode == "TCP Client" ? std::vector<std::uint8_t>{}
                                                                      : device.registrationBytes;
            auto [it, inserted] = definitions.try_emplace(
                key, DtuDefinition{
                         key, device.linkId, device.targetId, device.protocol, registration, {}});
            auto& definition = it->second;
            if (!inserted &&
                (definition.linkId != device.linkId || definition.targetId != device.targetId ||
                 definition.protocol != device.protocol ||
                 definition.registrationBytes != registration))
                throw std::runtime_error("DTU definition conflict: " + key);
            if (std::any_of(definition.devices.begin(), definition.devices.end(),
                            [&](const auto& current) { return current.slaveId == device.slaveId; }))
                throw std::runtime_error("DTU slave ID conflict: " + key);
            definition.devices.push_back(device);
            if (inserted)
                byLink[device.linkId].push_back(key);
        }
        std::lock_guard lock(mutex_);
        definitions_ = std::move(definitions);
        byLink_ = std::move(byLink);
    }

    [[nodiscard]] std::vector<DtuDefinition> byLink(std::string_view linkId) const {
        std::vector<DtuDefinition> result;
        std::lock_guard lock(mutex_);
        const auto keys = byLink_.find(std::string(linkId));
        if (keys == byLink_.end())
            return result;
        result.reserve(keys->second.size());
        for (const auto& key : keys->second) {
            const auto definition = definitions_.find(key);
            if (definition != definitions_.end())
                result.push_back(definition->second);
        }
        return result;
    }

    [[nodiscard]] std::optional<DtuDefinition> find(std::string_view key) const {
        std::lock_guard lock(mutex_);
        const auto definition = definitions_.find(std::string(key));
        if (definition == definitions_.end())
            return std::nullopt;
        return definition->second;
    }

    [[nodiscard]] std::optional<DtuDefinition> findClientTarget(std::string_view linkId,
                                                                std::string_view targetId) const {
        const auto definitions = byLink(linkId);
        const auto match = std::find_if(
            definitions.begin(), definitions.end(),
            [targetId](const auto& definition) { return definition.targetId == targetId; });
        if (match == definitions.end())
            return std::nullopt;
        return *match;
    }

    [[nodiscard]] RegistrationMatch
    matchRegistration(std::string_view linkId, const std::vector<std::uint8_t>& bytes) const {
        const auto definitions = byLink(linkId);
        std::optional<RegistrationMatch> exact;
        std::optional<RegistrationMatch> prefix;
        for (const auto& definition : definitions) {
            const auto& registration = definition.registrationBytes;
            if (registration.empty())
                continue;
            if (bytes == registration) {
                if (exact && exact->dtu.key != definition.key)
                    return {RegistrationMatchKind::Conflict, {}, bytes};
                exact = RegistrationMatch{RegistrationMatchKind::StandaloneFrame, definition, {}};
                continue;
            }
            if (bytes.size() > registration.size() &&
                std::equal(registration.begin(), registration.end(), bytes.begin())) {
                if (prefix && prefix->dtu.key != definition.key)
                    return {RegistrationMatchKind::Conflict, {}, bytes};
                prefix = RegistrationMatch{
                    RegistrationMatchKind::PrefixedPayload,
                    definition,
                    {bytes.begin() + static_cast<std::ptrdiff_t>(registration.size()),
                     bytes.end()}};
            }
        }
        if (exact)
            return *exact;
        if (prefix)
            return *prefix;
        return {};
    }

  private:
    static std::string makeKey(const DeviceDefinition& device) {
        if (device.linkMode == "TCP Client" && !device.targetId.empty())
            return device.linkId + ":TARGET:" + device.targetId;
        if (device.registrationBytes.empty())
            return device.linkId + ":NO_REG:" + device.id;
        return device.linkId + ":" + service::bridge::toHex(device.registrationBytes);
    }

    mutable std::mutex mutex_;
    std::map<std::string, DtuDefinition> definitions_;
    std::map<std::string, std::vector<std::string>> byLink_;
};

} // namespace service::southbridge
