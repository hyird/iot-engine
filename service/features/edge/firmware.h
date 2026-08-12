#pragma once

#include <cstdint>
#include <string_view>

#include "service/features/edge/protocol.h"

namespace service::edge::firmware {

inline bool populateUpdateRequest(pb::FirmwareUpdateRequest& request,
                                  std::string_view requestId,
                                  std::string_view downloadUrl,
                                  std::string_view sha256,
                                  std::uint64_t sizeBytes,
                                  bool keepSettings) {
    if (requestId.size() != 16 || downloadUrl.empty() || downloadUrl.size() > 512 ||
        sha256.size() != 32 || sizeBytes == 0)
        return false;
    request.set_request_id(requestId);
    request.set_download_url(downloadUrl);
    request.set_sha256(sha256);
    request.set_size_bytes(sizeBytes);
    request.set_keep_settings(keepSettings);
    return true;
}

} // namespace service::edge::firmware
