#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

struct StreamStatus {
    std::string app;
    std::string stream;
    std::string schema;
    bool online{false};
    int readerCount{0};
};

class StreamRegistry {
public:
    void updateStreamChanged(const std::string& app, const std::string& stream, const std::string& schema, bool online);
    void updateNoneReader(const std::string& app, const std::string& stream, const std::string& schema);
    std::optional<StreamStatus> findStream(const std::string& stream) const;
    std::vector<StreamStatus> listStreams() const;

private:
    static std::string keyFor(const std::string& app, const std::string& stream, const std::string& schema);

    mutable std::mutex mutex_;
    std::unordered_map<std::string, StreamStatus> streams_;
};
