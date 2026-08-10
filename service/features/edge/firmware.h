#pragma once

#include <cstdint>
#include <string_view>

#include "service/features/edge/protocol.h"

namespace service::edge::firmware {

inline bool rebootConfirmsTarget(std::string_view reportedVersion,
                                 std::string_view targetVersion) noexcept {
    return !reportedVersion.empty() && reportedVersion == targetVersion;
}

inline bool populateUpdateRequest(pb::FirmwareUpdateRequest& request,
                                  std::string_view requestId,
                                  std::string_view downloadUrl,
                                  std::string_view sha256,
                                  std::uint64_t sizeBytes,
                                  std::string_view version,
                                  bool keepSettings) {
    if (requestId.size() != 16 || downloadUrl.empty() || downloadUrl.size() > 512 ||
        sha256.size() != 32 || version.empty() || version.size() > 64)
        return false;
    request.set_request_id(requestId);
    request.set_download_url(downloadUrl);
    request.set_sha256(sha256);
    request.set_size_bytes(sizeBytes);
    request.set_version(version);
    request.set_keep_settings(keepSettings);
    return true;
}

} // namespace service::edge::firmware
