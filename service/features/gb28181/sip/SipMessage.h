#pragma once

#include <optional>
#include <string>
#include <unordered_map>

class SipMessage {
public:
    std::string startLine;
    std::string method;
    std::string requestUri;
    int statusCode{0};
    std::string reasonPhrase;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    static std::optional<SipMessage> parse(const std::string& raw);
    std::string header(const std::string& name) const;
};
