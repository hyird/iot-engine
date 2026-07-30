#include "media/StreamRegistry.h"

void StreamRegistry::updateStreamChanged(const std::string& app, const std::string& stream,
                                         const std::string& schema, bool online,
                                         int readerCount) {
    auto& status = streams_[keyFor(app, stream, schema)];
    status.app = app;
    status.stream = stream;
    status.schema = schema;
    status.online = online;
    status.readerCount = readerCount;
    if (observer_)
        observer_(status);
}

void StreamRegistry::updateNoneReader(const std::string& app, const std::string& stream, const std::string& schema) {
    auto& status = streams_[keyFor(app, stream, schema)];
    status.app = app;
    status.stream = stream;
    status.schema = schema;
    status.online = true;
    status.readerCount = 0;
    if (observer_)
        observer_(status);
}

std::optional<StreamStatus> StreamRegistry::findStream(const std::string& stream) const {
    std::optional<StreamStatus> match;
    for (const auto& [_, status] : streams_) {
        if (identity(status.app, status.stream, status.schema) == stream)
            return status;
        if (status.stream != stream)
            continue;
        if (match)
            return std::nullopt;
        match = status;
    }
    return match;
}

std::vector<StreamStatus> StreamRegistry::listStreams() const {
    std::vector<StreamStatus> result;
    result.reserve(streams_.size());
    for (const auto& [_, status] : streams_) {
        result.push_back(status);
    }
    return result;
}

std::string StreamRegistry::keyFor(const std::string& app, const std::string& stream, const std::string& schema) {
    return app + "\n" + stream + "\n" + schema;
}

std::string StreamRegistry::identity(const std::string& app,
                                     const std::string& stream,
                                     const std::string& schema) {
    return app + ":" + schema + ":" + stream;
}

void StreamRegistry::replace(std::vector<StreamStatus> streams) {
    streams_.clear();
    for (auto& stream : streams)
        streams_.emplace(keyFor(stream.app, stream.stream, stream.schema),
                         std::move(stream));
}
