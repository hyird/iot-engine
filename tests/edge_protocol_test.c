#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "edge_protocol.h"

static void require(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "edge protocol test failed: %s\n", message);
        exit(1);
    }
}

static void test_imei(void) {
    require(edge_protocol_validate_imei("490154203237518"), "valid IMEI rejected");
    require(!edge_protocol_validate_imei("490154203237519"), "bad check digit accepted");
    require(!edge_protocol_validate_imei("49015420323751"), "short IMEI accepted");
    require(!edge_protocol_validate_imei("49015420323751A"), "non-digit IMEI accepted");
}

static void test_hello_round_trip(void) {
    const uint8_t platform_id[16] = {0x01, 0x02, 0x03, 0x04};
    const uint8_t random_bytes[10] = {0x10, 0x11, 0x12, 0x13, 0x14,
                                      0x15, 0x16, 0x17, 0x18, 0x19};
    iot_edge_v1_Envelope envelope;
    require(edge_protocol_init_envelope(&envelope, platform_id, NULL, 0U, 1U,
                                        1784688000123LL, random_bytes),
            "envelope initialization failed");
    envelope.which_payload = iot_edge_v1_Envelope_hello_tag;
    strcpy(envelope.payload.hello.imei, "490154203237518");
    strcpy(envelope.payload.hello.model, "openwrt-test");
    strcpy(envelope.payload.hello.software_version, "0.1.0");
    envelope.payload.hello.supported_protocol_versions_count = 1U;
    envelope.payload.hello.supported_protocol_versions[0] = EDGENODE_PROTOCOL_VERSION;

    uint8_t encoded[EDGENODE_MAX_WS_MESSAGE];
    size_t encoded_size = 0U;
    const char *error = NULL;
    require(edge_protocol_encode(&envelope, encoded, sizeof(encoded), &encoded_size, &error),
            error != NULL ? error : "encode failed");
    require(encoded_size > 0U, "empty encoded envelope");

    iot_edge_v1_Envelope decoded;
    require(edge_protocol_decode(encoded, encoded_size, &decoded, &error),
            error != NULL ? error : "decode failed");
    require(decoded.which_payload == iot_edge_v1_Envelope_hello_tag, "wrong payload tag");
    require(strcmp(decoded.payload.hello.imei, "490154203237518") == 0,
            "IMEI changed during round trip");
    require(decoded.message_id.size == 16U && (decoded.message_id.bytes[6] >> 4U) == 7U,
            "message id is not UUIDv7");
    require((decoded.message_id.bytes[8] & 0xc0U) == 0x80U, "bad UUID variant");
}

static void test_reject_text_or_oversized_input(void) {
    iot_edge_v1_Envelope decoded;
    const char *error = NULL;
    const uint8_t json[] = "{\"type\":\"hello\"}";
    require(!edge_protocol_decode(json, sizeof(json) - 1U, &decoded, &error),
            "JSON WebSocket body was accepted as protobuf");
    require(!edge_protocol_decode(json, EDGENODE_MAX_WS_MESSAGE + 1U, &decoded, &error),
            "oversized WebSocket body was accepted");
}

int main(void) {
    test_imei();
    test_hello_round_trip();
    test_reject_text_or_oversized_input();
    puts("edge protocol tests passed");
    return 0;
}
