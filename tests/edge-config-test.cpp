#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "service/config/schema.h"
#include "service/features/edge/config.h"

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

void testPacketBytesRejectInvalidHex() {
    const auto bytes =
        service::edge::config::detail::packetBytes("HEX", "01 0a FF", "heartbeat_payload");
    require(bytes == std::vector<std::uint8_t>{0x01, 0x0a, 0xff},
            "edge config HEX packet changed valid bytes");

    bool rejectedBadDigit = false;
    try {
        (void)service::edge::config::detail::packetBytes("HEX", "0G", "heartbeat_payload");
    } catch (const std::runtime_error& error) {
        rejectedBadDigit =
            std::string_view(error.what()).find("invalid edge config hex: heartbeat_payload") !=
            std::string_view::npos;
    }
    require(rejectedBadDigit, "edge config accepted a HEX packet with a bad digit");

    bool rejectedOddNibble = false;
    try {
        (void)service::edge::config::detail::packetBytes("HEX", "ABC", "heartbeat_payload");
    } catch (const std::runtime_error& error) {
        rejectedOddNibble =
            std::string_view(error.what()).find("invalid edge config hex: heartbeat_payload") !=
            std::string_view::npos;
    }
    require(rejectedOddNibble, "edge config accepted a HEX packet with an odd nibble");
}

void testPacketBytesRejectUnknownMode() {
    bool rejected = false;
    try {
        (void)service::edge::config::detail::packetBytes("BASE64", "AA==", "heartbeat_payload");
    } catch (const std::runtime_error& error) {
        rejected =
            std::string_view(error.what()).find(
                "invalid edge config packet mode: heartbeat_payload") != std::string_view::npos;
    }
    require(rejected, "edge config accepted an unsupported packet mode");
}

void testReplaceQueueToleratesCorruptRevisionKey() {
    constexpr std::string_view script = service::edge::config::detail::kReplaceQueueScript;
    require(script.find("tonumber(redis.call('GET', KEYS[2]) or '0') or 0") !=
                std::string_view::npos,
            "edge config replaceQueue can compare nil when Redis revision key is corrupt");
}

void testNumberRejectsTrailingGarbage() {
    require(service::edge::config::detail::number("1.25", -1.0) == 1.25,
            "edge config decimal parser changed valid decimal");
    require(service::edge::config::detail::number("1.25x", -1.0) == -1.0,
            "edge config decimal parser accepted trailing garbage");
}

void testRevisionSqlGuardsCorruptNodeJson() {
    using namespace service::edge::config::detail;
    require(kQueueSnapshotSql.find("COALESCE((status->'config'->>'desiredVersion')::bigint") ==
                std::string_view::npos,
            "edge config queueSnapshot directly casts desiredVersion");
    require(kQueueSnapshotSql.find("COALESCE((status->'config'->>'activeVersion')::bigint") ==
                std::string_view::npos,
            "edge config queueSnapshot directly casts activeVersion");
    require(kQueueSnapshotSql.find("COALESCE((capability->>'deviceConfig')::boolean") ==
                std::string_view::npos,
            "edge config queueSnapshot directly casts deviceConfig capability");
    require(kRequeueDesiredSql.find("COALESCE((status->'config'->>'desiredVersion')::bigint") ==
                std::string_view::npos,
            "edge config requeue directly casts desiredVersion");
    require(kRequeuePendingSql.find("COALESCE((status->'config'->>'desiredVersion')::bigint") ==
                std::string_view::npos,
            "edge config requeue update directly casts desiredVersion");
    require(kRejectBuildSql.find("COALESCE((status->'config'->>'desiredVersion')::bigint") ==
                std::string_view::npos,
            "edge config rejectBuild directly casts desiredVersion");
    require(kQueueSnapshotSql.find("status->'config'->>'desiredVersion' ~ '^-?[0-9]{1,18}$'") !=
                std::string_view::npos,
            "edge config queueSnapshot does not guard desiredVersion");
    require(kQueueSnapshotSql.find("CASE lower(COALESCE(capability->>'deviceConfig', ''))") !=
                std::string_view::npos,
            "edge config queueSnapshot does not guard deviceConfig capability");
}

void testBuildItemSqlAvoidsJsonCasts() {
    using namespace service::edge::config::detail;
    require(kBuildItemsSql.find("(p.config->>'readInterval')::numeric") ==
                std::string_view::npos,
            "edge config buildItems directly casts readInterval");
    require(kBuildItemsSql.find("(d.protocol_params->>'online_timeout')::integer") ==
                std::string_view::npos,
            "edge config buildItems directly casts online_timeout");
    require(kBuildItemsSql.find("(l.endpoint->>'port')::integer") == std::string_view::npos,
            "edge config buildItems directly casts endpoint port");
    require(kBuildItemsSql.find("(l.endpoint->>'rs485')::boolean") == std::string_view::npos,
            "edge config buildItems directly casts rs485");
    require(kBuildItemsSql.find("(p.config->'packet'->>'mergeGap')::integer") ==
                std::string_view::npos,
            "edge config buildItems directly casts packet mergeGap");
    require(kBuildItemsSql.find("(p.config->'connection'->>'rack')::integer") ==
                std::string_view::npos,
            "edge config buildItems directly casts S7 rack");
    require(kBuildItemsSql.find("registration") == std::string_view::npos,
            "edge config still exports a device registration payload");
    require(kAppendModbusSql.find("(item->>'address')::integer") == std::string_view::npos,
            "edge config Modbus query directly casts address");
    require(kAppendModbusSql.find("(item->>'scale')::numeric") == std::string_view::npos,
            "edge config Modbus query directly casts scale");
    require(kAppendModbusSql.find("(item->>'writable')::boolean") == std::string_view::npos,
            "edge config Modbus query directly casts writable");
    require(kAppendS7Sql.find("(item->>'start')::integer") == std::string_view::npos,
            "edge config S7 query directly casts start");
    require(kAppendS7Sql.find("(item->>'writable')::boolean") == std::string_view::npos,
            "edge config S7 query directly casts writable");
    require(kAppendSl651ElementsSql.find("(element->>'length')::integer") ==
                std::string_view::npos,
            "edge config SL651 query directly casts length");
    require(kBuildItemsSql.find("COALESCE(NULLIF(p.config->>'readInterval', ''), '1')") !=
                std::string_view::npos,
            "edge config buildItems does not use the unified readInterval for reporting");
    require(kBuildItemsSql.find("pollInterval") == std::string_view::npos,
            "edge config buildItems still reads the retired pollInterval field");
    require(kBuildItemsSql.find("p.config->>'commandFastReadDuration'") !=
                std::string_view::npos,
            "edge config buildItems ignores the configured fast-read window");
    require(kBuildItemsSql.find("p.config->>'commandFastReadInterval'") !=
                std::string_view::npos,
            "edge config buildItems ignores the configured fast-read interval");
    require(kAppendModbusSql.find("COALESCE(NULLIF(item->>'scale', ''), '1')") !=
                std::string_view::npos,
            "edge config Modbus query does not leave scale for strict C++ parsing");
}

void testReadIntervalMigrationRemovesLegacyField() {
    const auto migration = std::find_if(
        service::config::kSchemaMigrations.begin(), service::config::kSchemaMigrations.end(),
        [](const auto& value) { return value.id() == "0026_unify_protocol_read_interval"; });
    require(migration != service::config::kSchemaMigrations.end(),
            "readInterval migration is missing");
    require(migration->sql().find("config - 'pollInterval'") != std::string_view::npos,
            "readInterval migration does not remove pollInterval");
    require(migration->sql().find("'{readInterval}'") != std::string_view::npos,
            "readInterval migration does not write the canonical field");
    require(migration->sql().find("ck_protocol_config_no_poll_interval") !=
                std::string_view::npos,
            "schema does not prevent pollInterval from returning");
}

void testStoragePolicyMigrationRemovesLegacyField() {
    const auto migration = std::find_if(
        service::config::kSchemaMigrations.begin(), service::config::kSchemaMigrations.end(),
        [](const auto& value) { return value.id() == "0027_unify_protocol_storage_policy"; });
    require(migration != service::config::kSchemaMigrations.end(),
            "storagePolicy migration is missing");
    require(migration->sql().find("config - 'storageInterval'") != std::string_view::npos &&
                migration->sql().find("'{storagePolicy}'") != std::string_view::npos,
            "storagePolicy migration does not replace the retired interval field");
    require(migration->sql().find("ELSE 'report'") != std::string_view::npos,
            "storagePolicy migration can silently reduce stored history");
    require(migration->sql().find("ck_protocol_config_storage_policy") !=
                std::string_view::npos &&
                migration->sql().find("COALESCE(config->>'storagePolicy' IN") !=
                    std::string_view::npos,
            "schema does not enforce a canonical storage policy");
}

void testEnrollmentMigrationRemovesRejectedState() {
    const auto& migration = service::config::kSchemaMigrations.back();
    require(migration.id() == "0028_remove_edge_enrollment_rejection",
            "enrollment migration is not the latest schema migration");
    require(migration.sql().find("WHERE enrollment_status = 'rejected'") !=
                std::string_view::npos,
            "enrollment migration does not migrate rejected registrations");
    require(migration.sql().find("IN ('pending', 'approved')") !=
                std::string_view::npos,
            "schema still permits rejected registrations");
}

} // namespace

int main() {
    try {
        testPacketBytesRejectInvalidHex();
        testPacketBytesRejectUnknownMode();
        testReplaceQueueToleratesCorruptRevisionKey();
        testNumberRejectsTrailingGarbage();
        testRevisionSqlGuardsCorruptNodeJson();
        testBuildItemSqlAvoidsJsonCasts();
        testReadIntervalMigrationRemovesLegacyField();
        testStoragePolicyMigrationRemovesLegacyField();
        testEnrollmentMigrationRemovesRejectedState();
        std::cout << "edge config tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "edge config test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
