#pragma once

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "service/features/edge/protocol.h"

namespace service::edge::firmware {

inline constexpr std::size_t kChunkSize{8192};

inline bool populateUpdateRequest(pb::FirmwareUpdateRequest& request,
                                  std::string_view requestId,
                                  std::string_view downloadUrl,
                                  std::string_view sha256,
                                  std::uint64_t sizeBytes,
                                  bool keepSettings) {
    if (requestId.size() != 16 || downloadUrl.size() > 512 ||
        sha256.size() != 32 || sizeBytes == 0)
        return false;
    request.set_request_id(requestId);
    request.set_download_url(downloadUrl);
    request.set_sha256(sha256);
    request.set_size_bytes(sizeBytes);
    request.set_keep_settings(keepSettings);
    return true;
}

struct ChunkReadResult final {
    std::string data;
    std::string error;
    bool eof{};
};

inline ChunkReadResult readChunk(const std::filesystem::path& path,
                                 std::uint64_t expectedSize,
                                 std::uint64_t offset) {
    ChunkReadResult result;
    std::error_code error;
    const auto actualSize = std::filesystem::file_size(path, error);
    if (error || actualSize != expectedSize) {
        result.error = "firmware storage size mismatch";
        return result;
    }
    if (offset >= expectedSize) {
        result.error = "firmware chunk offset is outside the image";
        return result;
    }
    const auto remaining = expectedSize - offset;
    const auto size = static_cast<std::size_t>(
        std::min<std::uint64_t>(remaining, kChunkSize));
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = "firmware storage cannot be opened";
        return result;
    }
    input.seekg(static_cast<std::streamoff>(offset));
    result.data.resize(size);
    input.read(result.data.data(), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        result.data.clear();
        result.error = "firmware storage read failed";
        return result;
    }
    result.eof = offset + size == expectedSize;
    return result;
}

} // namespace service::edge::firmware
