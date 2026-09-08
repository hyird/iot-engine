#pragma once

#include <string>
#include "service/common/uuid.h"

namespace service::runtime {

// Identifies a process incarnation, never reused by a restarted process. A socket
// address is (instanceId, workerIndex, sessionEpoch), not a local worker number.
inline const std::string& instanceId() {
    static const std::string id = service::common::nextUuidV7();
    return id;
}

} // namespace service::runtime
