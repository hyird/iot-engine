#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace service::access::webhook {

    struct Target final {
        std::string id;
        std::string accessKeyId;
        std::string url;
        std::string secret;
        std::string headers;
        std::int64_t timeout{5};
        bool skipTlsVerify{false};
        std::int64_t expiresAtMs{0};
    };

    struct DeviceCatalog final {
        std::string name;
        std::string code;
        std::map<std::string, std::vector<Target>, std::less<>> targets;
    };

    using Catalog = std::map<std::string, DeviceCatalog, std::less<>>;

} // namespace service::access::webhook
