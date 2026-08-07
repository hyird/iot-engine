#include <iostream>
#include <stdexcept>
#include <string>

#include "service/features/access/contract.h"
#include "service/features/access/event.h"
#include "service/features/access/session.h"
#include "service/features/event/config.h"

namespace access_contract = service::access;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main() {
    try {
        require(access_contract::sha256("abc") ==
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                "SHA-256 contract changed");
        require(access_contract::hmacSha256("key", "The quick brown fox jumps over the lazy dog") ==
                    "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8",
                "HMAC-SHA256 contract changed");
        require(access_contract::jsonQuoted("a\n\"b") == "\"a\\n\\\"b\"", "JSON escaping contract changed");
        require(access_contract::webhookEnvelope("device.data.reported", "1970-01-01T00:00:00Z",
                                      "delivery-1",
                                      R"({"device":{"id":"device-1"},"points":[]})") ==
                    R"({"event":"device.data.reported","time":"1970-01-01T00:00:00Z","deliveryId":"delivery-1","data":{"device":{"id":"device-1"},"points":[]}})",
                "Webhook envelope contract changed");
        require(access_contract::iso8601(0) == "1970-01-01T00:00:00Z", "UTC timestamp contract changed");
        require(access_contract::iso8601(999) == "1970-01-01T00:00:00Z",
                "UTC timestamp must use seconds precision");
        require(access_contract::iso8601(-1) == "1969-12-31T23:59:59Z",
                "UTC timestamp must floor negative milliseconds");
        const auto now = access_contract::nowIso8601();
        require(now.size() == 20 && now[4] == '-' && now[7] == '-' && now[10] == 'T' &&
                    now[13] == ':' && now[16] == ':' && now[19] == 'Z',
                "current timestamp must use the RFC 3339 wire format");
        const auto key = access_contract::generateAccessKey();
        require(key.starts_with("ak_") && key.size() == 51, "AccessKey format contract changed");
        require(access_contract::supportedScope("device:command") &&
                    !access_contract::supportedScope("device:admin"),
                "scope allowlist contract changed");
        require(access_contract::supportedEvent("device.data.reported") &&
                    !access_contract::supportedEvent("device.deleted"),
                "event allowlist contract changed");
        const auto dataPublication = service::access::event::publicationKey(
            "019fd9f6-4be5-7272-a194-9e571bce848d", "device.data.reported");
        require(dataPublication ==
                    "iot:open-access:event:published:device.data.reported:"
                    "019fd9f6-4be5-7272-a194-9e571bce848d",
                "open-access publication idempotency key changed");
        require(dataPublication != service::access::event::publicationKey(
                                       "019fd9f6-4be5-7272-a194-9e571bce848d",
                                       "device.command.responded"),
                "different event types incorrectly share an idempotency key");
        require(service::message::kRuntimeConfigChangesStream !=
                    service::message::kWebhookCatalogChangesStream,
                "independent config consumers must not XDEL from a shared Stream");
        const auto encodedSession = service::access::session::encode(
            "019fd9f6-4be5-7272-a194-9e571bce848d", "production key", "enabled",
            "4102444800000", R"(["device:realtime","device:history"])",
            R"(["019fd9f6-4be5-7272-a194-9e571bce848e"])");
        const auto decodedSession = service::access::session::decode(encodedSession);
        require(decodedSession.id == "019fd9f6-4be5-7272-a194-9e571bce848d" &&
                    decodedSession.name == "production key" &&
                    decodedSession.status == "enabled" &&
                    decodedSession.expiresAtMs == 4102444800000LL &&
                    decodedSession.scopes.contains("device:realtime") &&
                    decodedSession.scopes.contains("device:history") &&
                    decodedSession.deviceIds.contains(
                        "019fd9f6-4be5-7272-a194-9e571bce848e"),
                "projected AccessKey session round-trip changed");
        require(!service::access::session::expired(decodedSession, 4102444799999LL) &&
                    service::access::session::expired(decodedSession, 4102444800000LL),
                "projected AccessKey expiration boundary changed");
        std::cout << "open access tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "open access test failed: " << error.what() << '\n';
        return 1;
    }
}
