#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "service/common/message/shard.h"

namespace service::access::stream {

// A fixed partition count keeps Stream ownership stable when SERVICE_WORKERS changes.
// The server currently caps Service Workers at the same value, so every Worker owns at
// least one partition and partitions can be reassigned without moving queued messages.
inline constexpr std::size_t kPartitionCount = service::message::shard::kCount;

inline constexpr std::string_view kEventBase{"iot:channel:open-access:event"};
inline constexpr std::string_view kAuditBase{"iot:channel:open-access:audit"};
inline constexpr std::string_view kDeliveryResultBase{
    "iot:channel:open-access:delivery-result"};
inline constexpr std::string_view kCatalogChangesBase{
    "iot:channel:open-access:config-change"};
inline constexpr std::string_view kSessionChangesBase{
    "iot:channel:open-access:session-change"};

inline std::size_t partition(std::string_view key) noexcept {
    return service::message::shard::index(key);
}

inline std::string partitioned(std::string_view base, std::size_t partitionIndex) {
    return std::string(base) + ":" + std::to_string(partitionIndex);
}

inline std::string event(std::size_t partitionIndex) {
    return partitioned(kEventBase, partitionIndex);
}

inline std::string event(std::string_view deviceId) {
    return event(partition(deviceId));
}

inline std::string audit(std::size_t partitionIndex) {
    return partitioned(kAuditBase, partitionIndex);
}

inline std::string audit(std::string_view routingKey) {
    return audit(partition(routingKey));
}

inline std::string deliveryResult(std::size_t partitionIndex) {
    return partitioned(kDeliveryResultBase, partitionIndex);
}

inline std::string deliveryResult(std::string_view deviceId) {
    return deliveryResult(partition(deviceId));
}

inline std::string catalogChanges(std::size_t workerIndex) {
    return partitioned(kCatalogChangesBase, workerIndex);
}

inline std::string sessionChanges(std::size_t partitionIndex) {
    return partitioned(kSessionChangesBase, partitionIndex);
}

inline std::string sessionChanges(std::string_view aggregateId) {
    return sessionChanges(partition(aggregateId));
}

} // namespace service::access::stream
