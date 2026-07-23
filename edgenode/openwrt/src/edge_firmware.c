#include "edge_firmware.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <mbedtls/sha256.h>

#include "edge_process.h"

#define FIRMWARE_IMAGE "/tmp/edgenode/firmware.bin"
#define FIRMWARE_LOCK "/tmp/edgenode/firmware.lock"
#define FIRMWARE_STATUS_MAGIC 0x45444745U

typedef struct {
    uint32_t magic;
    uint8_t request_id[16];
    uint32_t state;
    char message[257];
} firmware_status;

static void set_error(char *output, size_t capacity, const char *message) {
    if (output != NULL && capacity != 0U)
        snprintf(output, capacity, "%s", message != NULL ? message : "firmware error");
}

static void status_path(const uint8_t platform_id[16], char output[96]) {
    static const char hex[] = "0123456789abcdef";
    size_t offset = (size_t)snprintf(output, 96U, "/tmp/edgenode/firmware-");
    for (size_t index = 0; index < 16U && offset + 2U < 96U; ++index) {
        output[offset++] = hex[platform_id[index] >> 4U];
        output[offset++] = hex[platform_id[index] & 0x0FU];
    }
    snprintf(output + offset, 96U - offset, ".status");
}

static bool write_status(const uint8_t platform_id[16], const uint8_t request_id[16],
                         iot_edge_v1_FirmwareUpdateState state, const char *message) {
    char path[96];
    char temporary[104];
    status_path(platform_id, path);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    firmware_status value;
    memset(&value, 0, sizeof(value));
    value.magic = FIRMWARE_STATUS_MAGIC;
    memcpy(value.request_id, request_id, sizeof(value.request_id));
    value.state = (uint32_t)state;
    snprintf(value.message, sizeof(value.message), "%s", message != NULL ? message : "");
    const int output = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (output < 0)
        return false;
    const ssize_t written = write(output, &value, sizeof(value));
    const bool ok = written == (ssize_t)sizeof(value) && fsync(output) == 0;
    close(output);
    if (!ok || rename(temporary, path) != 0) {
        unlink(temporary);
        return false;
    }
    return true;
}

static bool sha256_file(const char *path, uint8_t output[32]) {
    FILE *input = fopen(path, "rb");
    if (input == NULL)
        return false;
    mbedtls_sha256_context context;
    mbedtls_sha256_init(&context);
    bool ok = mbedtls_sha256_starts_ret(&context, 0) == 0;
    uint8_t buffer[4096];
    while (ok) {
        const size_t size = fread(buffer, 1U, sizeof(buffer), input);
        if (size != 0U && mbedtls_sha256_update_ret(&context, buffer, size) != 0)
            ok = false;
        if (size < sizeof(buffer)) {
            if (ferror(input) != 0)
                ok = false;
            break;
        }
    }
    if (ok)
        ok = mbedtls_sha256_finish_ret(&context, output) == 0;
    mbedtls_sha256_free(&context);
    fclose(input);
    return ok;
}

static void firmware_child(const uint8_t platform_id[16],
                           const iot_edge_v1_FirmwareUpdateRequest *request) {
    const uint8_t *request_id = request->request_id.bytes;
    (void)write_status(platform_id, request_id,
                       iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_DOWNLOADING,
                       "downloading firmware");
    const char *download[] = {"uclient-fetch", "--no-check-certificate", "-O",
                              FIRMWARE_IMAGE, request->download_url, NULL};
    if (edge_process_run(download, -1, -1) != 0) {
        (void)write_status(platform_id, request_id,
                           iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                           "firmware download failed");
        unlink(FIRMWARE_LOCK);
        _exit(1);
    }
    struct stat info;
    if (stat(FIRMWARE_IMAGE, &info) != 0 || info.st_size < 0 ||
        (uint64_t)info.st_size != request->size_bytes) {
        (void)write_status(platform_id, request_id,
                           iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                           "firmware size mismatch");
        unlink(FIRMWARE_IMAGE);
        unlink(FIRMWARE_LOCK);
        _exit(1);
    }
    (void)write_status(platform_id, request_id,
                       iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_VERIFYING,
                       "verifying firmware sha256");
    uint8_t actual[32];
    if (!sha256_file(FIRMWARE_IMAGE, actual) ||
        memcmp(actual, request->sha256.bytes, sizeof(actual)) != 0) {
        (void)write_status(platform_id, request_id,
                           iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                           "firmware sha256 mismatch");
        unlink(FIRMWARE_IMAGE);
        unlink(FIRMWARE_LOCK);
        _exit(1);
    }
    (void)write_status(platform_id, request_id,
                       iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FLASHING,
                       "firmware verified; sysupgrade is starting");
    sleep(2U);
    const char *keep[] = {"sysupgrade", FIRMWARE_IMAGE, NULL};
    const char *reset[] = {"sysupgrade", "-n", FIRMWARE_IMAGE, NULL};
    const int result = edge_process_run(request->keep_settings ? keep : reset, -1, -1);
    (void)write_status(platform_id, request_id,
                       iot_edge_v1_FirmwareUpdateState_FIRMWARE_UPDATE_FAILED,
                       result == 0 ? "sysupgrade returned without reboot"
                                   : "sysupgrade rejected firmware");
    unlink(FIRMWARE_IMAGE);
    unlink(FIRMWARE_LOCK);
    _exit(1);
}

bool edge_firmware_start(const uint8_t platform_id[16],
                         const iot_edge_v1_FirmwareUpdateRequest *request,
                         char *error, size_t error_size) {
    if (platform_id == NULL || request == NULL || request->request_id.size != 16U ||
        request->sha256.size != 32U || request->size_bytes == 0U ||
        request->size_bytes > 128U * 1024U * 1024U ||
        (strncmp(request->download_url, "https://", 8U) != 0 &&
         strncmp(request->download_url, "http://", 7U) != 0)) {
        set_error(error, error_size, "firmware request is invalid");
        return false;
    }
    const int lock = open(FIRMWARE_LOCK, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (lock < 0) {
        set_error(error, error_size, "another firmware update is active");
        return false;
    }
    close(lock);
    const pid_t child = fork();
    if (child < 0) {
        unlink(FIRMWARE_LOCK);
        set_error(error, error_size, "cannot start firmware worker");
        return false;
    }
    if (child == 0)
        firmware_child(platform_id, request);
    return true;
}

bool edge_firmware_read_status(const uint8_t platform_id[16],
                               iot_edge_v1_FirmwareUpdateResult *result) {
    if (platform_id == NULL || result == NULL)
        return false;
    char path[96];
    status_path(platform_id, path);
    const int input = open(path, O_RDONLY);
    if (input < 0)
        return false;
    firmware_status value;
    const ssize_t size = read(input, &value, sizeof(value));
    close(input);
    if (size != (ssize_t)sizeof(value) || value.magic != FIRMWARE_STATUS_MAGIC)
        return false;
    unlink(path);
    memset(result, 0, sizeof(*result));
    result->request_id.size = 16U;
    memcpy(result->request_id.bytes, value.request_id, 16U);
    result->state = (iot_edge_v1_FirmwareUpdateState)value.state;
    snprintf(result->message, sizeof(result->message), "%s", value.message);
    return true;
}
