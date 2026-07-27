#include "media/StreamRegistry.h"

void StreamRegistry::updateStreamChanged(const std::string& app, const std::string& stream, const std::string& schema, bool online) {
    std::lock_guard lock(mutex_);
    auto& status = streams_[keyFor(app, stream, schema)];
    status.app = app;
    status.stream = stream;
    status.schema = schema;
    status.online = online;
}

void StreamRegistry::updateNoneReader(const std::string& app, const std::string& stream, const std::string& schema) {
    std::lock_guard lock(mutex_);
    auto& status = streams_[keyFor(app, stream, schema)];
    status.app = app;
    status.stream = stream;
    status.schema = schema;
    status.online = true;
    status.readerCount = 0;
}

std::optional<StreamStatus> StreamRegistry::findStream(const std::string& stream) const {
    std::lock_guard lock(mutex_);
    for (const auto& [_, status] : streams_) {
        if (status.stream == stream) {
            return status;
        }
    }
    return std::nullopt;
}

std::vector<StreamStatus> StreamRegistry::listStreams() const {
    std::lock_guard lock(mutex_);
    std::vector<StreamStatus> result;
    result.reserve(streams_.size());
    for (const auto& [_, status] : streams_) {
        result.push_back(status);
    }
    return result;
}

std::string StreamRegistry::keyFor(const std::string& app, const std::string& stream, const std::string& schema) {
    return app + "/" + stream + "/" + schema;
}
