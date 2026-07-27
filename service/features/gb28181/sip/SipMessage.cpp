#include "sip/SipMessage.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

std::string trim(std::string value) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

} // namespace

std::optional<SipMessage> SipMessage::parse(const std::string& raw) {
    const auto headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return std::nullopt;
    }

    SipMessage message;
    message.body = raw.substr(headerEnd + 4);

    std::istringstream lines(raw.substr(0, headerEnd));
    if (!std::getline(lines, message.startLine)) {
        return std::nullopt;
    }
    if (!message.startLine.empty() && message.startLine.back() == '\r') {
        message.startLine.pop_back();
    }

    std::istringstream start(message.startLine);
    if (message.startLine.rfind("SIP/2.0", 0) == 0) {
        std::string version;
        start >> version >> message.statusCode;
        std::getline(start, message.reasonPhrase);
        message.reasonPhrase = trim(message.reasonPhrase);
    } else {
        start >> message.method >> message.requestUri;
    }

    if (message.method.empty() && message.statusCode == 0) {
        return std::nullopt;
    }

    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        auto name = lower(trim(line.substr(0, colon)));
        auto value = trim(line.substr(colon + 1));
        message.headers[name] = value;
    }

    return message;
}

std::string SipMessage::header(const std::string& name) const {
    auto key = lower(name);
    const auto iter = headers.find(key);
    if (iter == headers.end()) {
        return {};
    }
    return iter->second;
}
