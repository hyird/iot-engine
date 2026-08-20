#pragma once

#include <optional>
#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
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
    using Observer = std::function<void(const StreamStatus&)>;

    explicit StreamRegistry(Observer observer = {})
        : observer_(std::move(observer)) {}

    void updateStreamChanged(const std::string& app, const std::string& stream,
                             const std::string& schema, bool online, int readerCount);
    void updateViewerCount(const std::string& stream, int viewerCount);
    std::optional<StreamStatus> findStream(const std::string& stream) const;
    std::vector<StreamStatus> listStreams() const;
    void replace(std::vector<StreamStatus> streams);
    static std::string identity(const std::string& app, const std::string& stream,
                                const std::string& schema);

private:
    static std::string keyFor(const std::string& app, const std::string& stream, const std::string& schema);

    std::unordered_map<std::string, StreamStatus> streams_;
    std::unordered_map<std::string, int> viewerCounts_;
    Observer observer_;
};
