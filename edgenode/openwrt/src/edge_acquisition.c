#define _GNU_SOURCE

#include "edge_acquisition.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <linux/serial.h>
#endif

#include "edge_device_runtime.h"
#include "edge_modbus.h"
#include "edge_protocol.h"
#include "edge_s7.h"

#define EDGE_IO_TIMEOUT_MS 800

typedef struct edge_acquisition_device edge_acquisition_device;

typedef struct {
    const iot_edge_v1_ConfigItem *item;
    iot_edge_v1_TelemetryValue value;
    bool valid;
} edge_acquisition_point;

struct edge_acquisition_device {
    edge_acquisition *owner;
    const iot_edge_v1_EndpointConfig *endpoint;
    const iot_edge_v1_DeviceConfig *config;
    edge_acquisition_point *points;
    size_t point_count;
    edge_device_runtime runtime;
    int fd;
    int listen_fd;
    uint16_t transaction;
    uint16_t s7_reference;
    uint16_t s7_pdu_length;
    int64_t observed_at_ms;
};

struct edge_acquisition {
    edge_acquisition_telemetry_callback telemetry;
    edge_acquisition_command_callback command;
    void *callback_context;
    edge_acquisition_device *devices;
    size_t device_count;
};

static void set_error(char *error, size_t size, const char *message) {
    if (error != NULL && size != 0U)
        snprintf(error, size, "%s", message);
}

static void copy_text(char *output, size_t capacity, const char *input) {
    if (capacity == 0U)
        return;
    snprintf(output, capacity, "%s", input != NULL ? input : "");
}

static bool same_id(const void *field, const uint8_t id[16]) {
    pb_size_t size = 0U;
    memcpy(&size, field, sizeof(size));
    return size == 16U && memcmp((const uint8_t *)field + sizeof(size), id, 16U) == 0;
}

static bool random_bytes(uint8_t *output, size_t size) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    size_t offset = 0U;
    while (offset < size) {
        const ssize_t count = read(fd, output + offset, size - offset);
        if (count <= 0) {
            close(fd);
            return false;
        }
        offset += (size_t)count;
    }
    close(fd);
    return true;
}

static void record_id(uint64_t now_ms, uint8_t output[16]) {
    uint8_t random[10] = {0};
    if (!random_bytes(random, sizeof(random))) {
        for (size_t index = 0; index < sizeof(random); ++index)
            random[index] = (uint8_t)(now_ms >> ((index % 8U) * 8U));
    }
    edge_protocol_uuid_v7(now_ms, random, output);
}

static int64_t current_ms(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_REALTIME, &value) != 0)
        return 0;
    return (int64_t)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static void close_fd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void close_on_exec(int fd) {
    const int flags = fcntl(fd, F_GETFD, 0);
    if (flags >= 0)
        (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

static int wait_fd(int fd, short events) {
    struct pollfd descriptor = {.fd = fd, .events = events};
    for (;;) {
        const int result = poll(&descriptor, 1U, EDGE_IO_TIMEOUT_MS);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            return -1;
        return (descriptor.revents & events) != 0 ? 0 : -1;
    }
}

static bool write_all(int fd, const uint8_t *data, size_t size) {
    size_t offset = 0U;
    while (offset < size) {
        if (wait_fd(fd, POLLOUT) != 0)
            return false;
        const ssize_t count = write(fd, data + offset, size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += (size_t)count;
    }
    return true;
}

static bool read_exact(int fd, uint8_t *data, size_t size) {
    size_t offset = 0U;
    while (offset < size) {
        if (wait_fd(fd, POLLIN) != 0)
            return false;
        const ssize_t count = read(fd, data + offset, size - offset);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += (size_t)count;
    }
    return true;
}

static speed_t baud_rate(uint32_t value) {
    switch (value) {
    case 300U: return B300;
    case 600U: return B600;
    case 1200U: return B1200;
    case 2400U: return B2400;
    case 4800U: return B4800;
    case 9600U: return B9600;
    case 19200U: return B19200;
    case 38400U: return B38400;
#ifdef B57600
    case 57600U: return B57600;
#endif
#ifdef B115200
    case 115200U: return B115200;
#endif
    default: return (speed_t)0;
    }
}

static int open_serial(const iot_edge_v1_SerialSettings *settings) {
    const speed_t speed = baud_rate(settings->baud_rate);
    if (speed == (speed_t)0)
        return -1;
    int fd = open(settings->channel, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return -1;
    struct termios options;
    if (tcgetattr(fd, &options) != 0) {
        close(fd);
        return -1;
    }
    cfmakeraw(&options);
    cfsetispeed(&options, speed);
    cfsetospeed(&options, speed);
    options.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
    options.c_cflag |= CLOCAL | CREAD;
    options.c_cflag |= settings->data_bits == 5U ? CS5
                       : settings->data_bits == 6U ? CS6
                       : settings->data_bits == 7U ? CS7 : CS8;
    if (strcmp(settings->parity, "even") == 0)
        options.c_cflag |= PARENB;
    else if (strcmp(settings->parity, "odd") == 0)
        options.c_cflag |= PARENB | PARODD;
    if (settings->stop_bits == 2U)
        options.c_cflag |= CSTOPB;
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &options) != 0) {
        close(fd);
        return -1;
    }
#if defined(__linux__) && defined(TIOCSRS485)
    if (settings->rs485) {
        struct serial_rs485 mode;
        memset(&mode, 0, sizeof(mode));
        mode.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;
        (void)ioctl(fd, TIOCSRS485, &mode);
    }
#endif
    return fd;
}

static bool socket_address(const char *ip, uint32_t port, struct sockaddr_in *address) {
    memset(address, 0, sizeof(*address));
    address->sin_family = AF_INET;
    address->sin_port = htons((uint16_t)port);
    return inet_pton(AF_INET, ip, &address->sin_addr) == 1;
}

static void bind_interface(int fd, const char *name) {
#ifdef SO_BINDTODEVICE
    if (name != NULL && name[0] != '\0')
        (void)setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, name, strlen(name) + 1U);
#else
    (void)fd;
    (void)name;
#endif
}

static int open_tcp_client(const iot_edge_v1_EndpointConfig *endpoint) {
    struct sockaddr_in address;
    if (!socket_address(endpoint->ip, endpoint->port, &address))
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    close_on_exec(fd);
    bind_interface(fd, endpoint->interface_name);
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        close(fd);
        return -1;
    }
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        if (errno != EINPROGRESS || wait_fd(fd, POLLOUT) != 0) {
            close(fd);
            return -1;
        }
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0) {
            close(fd);
            return -1;
        }
    }
    if (fcntl(fd, F_SETFL, flags) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int accept_tcp_client(edge_acquisition_device *device) {
    if (device->listen_fd < 0) {
        struct sockaddr_in address;
        if (!socket_address(device->endpoint->ip, device->endpoint->port, &address))
            return -1;
        device->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (device->listen_fd < 0)
            return -1;
        close_on_exec(device->listen_fd);
        const int flags = fcntl(device->listen_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(device->listen_fd, F_SETFL, flags | O_NONBLOCK) != 0) {
            close_fd(&device->listen_fd);
            return -1;
        }
        int enabled = 1;
        (void)setsockopt(device->listen_fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                         sizeof(enabled));
        bind_interface(device->listen_fd, device->endpoint->interface_name);
        if (bind(device->listen_fd, (const struct sockaddr *)&address, sizeof(address)) != 0 ||
            listen(device->listen_fd, 1) != 0) {
            close_fd(&device->listen_fd);
            return -1;
        }
    }
    int fd = accept(device->listen_fd, NULL, NULL);
    if (fd < 0)
        return -1;
    close_on_exec(fd);
    return fd;
}

static edge_io_result device_connect(void *context) {
    edge_acquisition_device *device = context;
    if (device->fd >= 0)
        return EDGE_IO_OK;
    if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL)
        device->fd = open_serial(&device->endpoint->serial);
    else if (device->endpoint->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER)
        device->fd = accept_tcp_client(device);
    else
        device->fd = open_tcp_client(device->endpoint);
    if (device->fd < 0)
        return EDGE_IO_OFFLINE;
    if (device->endpoint->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER &&
        device->config->registration_payload.size != 0U) {
        uint8_t registration[256];
        const size_t size = device->config->registration_payload.size;
        if (!read_exact(device->fd, registration, size) ||
            memcmp(registration, device->config->registration_payload.bytes, size) != 0) {
            close_fd(&device->fd);
            return EDGE_IO_PROTOCOL_ERROR;
        }
    }
    return EDGE_IO_OK;
}

static void device_disconnect(void *context) {
    edge_acquisition_device *device = context;
    close_fd(&device->fd);
    device->s7_pdu_length = 0U;
}

static bool receive_modbus(edge_acquisition_device *device, uint8_t *frame,
                            size_t capacity, size_t *size) {
    if (strcmp(device->config->modbus_mode, "RTU") == 0) {
        if (!read_exact(device->fd, frame, 3U))
            return false;
        size_t remaining;
        if ((frame[1] & 0x80U) != 0U)
            remaining = 2U;
        else if (frame[1] >= 1U && frame[1] <= 4U)
            remaining = (size_t)frame[2] + 2U;
        else
            remaining = 5U;
        if (3U + remaining > capacity || !read_exact(device->fd, frame + 3U, remaining))
            return false;
        *size = 3U + remaining;
        return true;
    }
    if (!read_exact(device->fd, frame, 7U))
        return false;
    const size_t length = ((size_t)frame[4] << 8U) | frame[5];
    if (length < 2U || length + 6U > capacity ||
        !read_exact(device->fd, frame + 7U, length - 1U))
        return false;
    *size = length + 6U;
    return true;
}

static bool receive_s7(edge_acquisition_device *device, uint8_t *frame,
                        size_t capacity, size_t *size) {
    if (!read_exact(device->fd, frame, 4U))
        return false;
    const size_t length = ((size_t)frame[2] << 8U) | frame[3];
    if (length < 7U || length > capacity || !read_exact(device->fd, frame + 4U, length - 4U))
        return false;
    *size = length;
    return true;
}

static bool exchange(edge_acquisition_device *device, const uint8_t *request,
                     size_t request_size, uint8_t *response, size_t capacity,
                     size_t *response_size) {
    if (device->endpoint->transport == iot_edge_v1_Transport_TRANSPORT_SERIAL)
        (void)tcflush(device->fd, TCIFLUSH);
    else if (device->endpoint->mode == iot_edge_v1_LinkMode_LINK_MODE_TCP_SERVER &&
             device->config->heartbeat_payload.size != 0U) {
        for (;;) {
            int available = 0;
            const size_t heartbeat_size = device->config->heartbeat_payload.size;
            if (ioctl(device->fd, FIONREAD, &available) != 0 ||
                available < (int)heartbeat_size)
                break;
            uint8_t heartbeat[256];
            const ssize_t peeked = recv(device->fd, heartbeat, heartbeat_size, MSG_PEEK);
            if (peeked != (ssize_t)heartbeat_size ||
                memcmp(heartbeat, device->config->heartbeat_payload.bytes,
                       heartbeat_size) != 0)
                break;
            if (!read_exact(device->fd, heartbeat, heartbeat_size))
                break;
        }
    }
    if (!write_all(device->fd, request, request_size)) {
        close_fd(&device->fd);
        return false;
    }
    const bool received = device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_S7
                              ? receive_s7(device, response, capacity, response_size)
                              : receive_modbus(device, response, capacity, response_size);
    if (!received)
        close_fd(&device->fd);
    return received;
}

static bool parse_hex16(const char *text, uint16_t *value) {
    if (text == NULL || text[0] == '\0')
        return false;
    char *end = NULL;
    errno = 0;
    const unsigned long parsed = strtoul(text, &end, 16);
    if (errno != 0 || end == text || *end != '\0' || parsed > 0xffffUL)
        return false;
    *value = (uint16_t)parsed;
    return true;
}

static edge_io_result device_handshake(void *context) {
    edge_acquisition_device *device = context;
    uint16_t local = 0x0100U;
    uint16_t remote = 0U;
    if (strcmp(device->config->s7_connection_mode, "TSAP") == 0) {
        if (!parse_hex16(device->config->s7_local_tsap, &local) ||
            !parse_hex16(device->config->s7_remote_tsap, &remote))
            return EDGE_IO_PROTOCOL_ERROR;
    } else {
        const uint16_t type = strcmp(device->config->s7_connection_type, "OP") == 0 ? 2U
                              : strcmp(device->config->s7_connection_type, "S7_BASIC") == 0
                                  ? 3U : 1U;
        remote = (uint16_t)((type << 8U) | ((device->config->s7_rack & 7U) << 5U) |
                            (device->config->s7_slot & 31U));
    }
    uint8_t request[EDGE_S7_MAX_FRAME];
    uint8_t response[EDGE_S7_MAX_FRAME];
    size_t response_size = 0U;
    size_t request_size = edge_s7_build_cotp_connect(local, remote, request, sizeof(request));
    if (request_size == 0U ||
        !exchange(device, request, request_size, response, sizeof(response), &response_size))
        return EDGE_IO_NO_RESPONSE;
    if (edge_s7_parse_cotp_confirm(response, response_size) != EDGE_S7_OK)
        return EDGE_IO_PROTOCOL_ERROR;
    const uint16_t reference = ++device->s7_reference;
    request_size = edge_s7_build_setup(reference, EDGE_S7_DEFAULT_PDU_LENGTH,
                                       request, sizeof(request));
    if (request_size == 0U ||
        !exchange(device, request, request_size, response, sizeof(response), &response_size))
        return EDGE_IO_NO_RESPONSE;
    return edge_s7_parse_setup(response, response_size, reference, &device->s7_pdu_length) ==
                   EDGE_S7_OK
               ? EDGE_IO_OK
               : EDGE_IO_PROTOCOL_ERROR;
}

static uint8_t modbus_function(const char *type) {
    if (strcmp(type, "COIL") == 0)
        return 1U;
    if (strcmp(type, "DISCRETE_INPUT") == 0)
        return 2U;
    if (strcmp(type, "HOLDING_REGISTER") == 0)
        return 3U;
    if (strcmp(type, "INPUT_REGISTER") == 0)
        return 4U;
    return 0U;
}

static edge_modbus_transport modbus_transport(const edge_acquisition_device *device) {
    return strcmp(device->config->modbus_mode, "RTU") == 0
               ? EDGE_MODBUS_RTU : EDGE_MODBUS_TCP;
}

static edge_s7_area s7_area(const char *area) {
    if (strcmp(area, "DB") == 0 || strcmp(area, "V") == 0)
        return EDGE_S7_AREA_DB;
    if (strcmp(area, "MK") == 0)
        return EDGE_S7_AREA_FLAGS;
    if (strcmp(area, "PE") == 0)
        return EDGE_S7_AREA_INPUTS;
    if (strcmp(area, "PA") == 0)
        return EDGE_S7_AREA_OUTPUTS;
    if (strcmp(area, "CT") == 0)
        return EDGE_S7_AREA_COUNTER;
    return EDGE_S7_AREA_TIMER;
}

static edge_s7_address s7_address(const iot_edge_v1_S7AreaConfig *point) {
    edge_s7_address address = {.area = s7_area(point->area),
                               .db_number = (uint16_t)(strcmp(point->area, "V") == 0
                                                          ? 1U : point->db_number),
                               .start_byte = point->start,
                               .start_bit = (uint8_t)point->start_bit,
                               .size = (uint16_t)point->size,
                               .bit_access = strcmp(point->data_type, "BOOL") == 0};
    return address;
}

static edge_io_result read_modbus_point(edge_acquisition_device *device,
                                         const iot_edge_v1_ModbusRegisterConfig *point,
                                         uint8_t *data, size_t capacity, size_t *data_size) {
    edge_modbus_request request = {
        .transport = modbus_transport(device),
        .transaction_id = ++device->transaction,
        .unit_id = (uint8_t)device->config->modbus_slave_id,
        .function = modbus_function(point->register_type),
        .address = (uint16_t)point->address,
        .quantity = (uint16_t)point->quantity};
    uint8_t output[EDGE_MODBUS_MAX_FRAME];
    uint8_t response[EDGE_MODBUS_MAX_FRAME];
    size_t output_size = 0U;
    size_t response_size = 0U;
    uint8_t exception = 0U;
    if (request.function == 0U ||
        edge_modbus_build_read(&request, output, sizeof(output), &output_size) != EDGE_MODBUS_OK)
        return EDGE_IO_PROTOCOL_ERROR;
    if (!exchange(device, output, output_size, response, sizeof(response), &response_size))
        return EDGE_IO_NO_RESPONSE;
    const edge_modbus_result parsed = edge_modbus_parse_response(
        &request, response, response_size, NULL, 0U, data, capacity, data_size, &exception);
    if (parsed != EDGE_MODBUS_OK)
        return EDGE_IO_PROTOCOL_ERROR;
    if ((request.function == 1U || request.function == 2U) && *data_size != 0U) {
        data[0] = (uint8_t)(data[0] & 1U);
        *data_size = 1U;
    }
    return EDGE_IO_OK;
}

static edge_io_result read_s7_point(edge_acquisition_device *device,
                                     const iot_edge_v1_S7AreaConfig *point,
                                     uint8_t *data, size_t capacity, size_t *data_size) {
    const edge_s7_address address = s7_address(point);
    const uint16_t reference = ++device->s7_reference;
    uint8_t output[EDGE_S7_MAX_FRAME];
    uint8_t response[EDGE_S7_MAX_FRAME];
    const size_t output_size = edge_s7_build_read(reference, &address, output, sizeof(output));
    size_t response_size = 0U;
    uint8_t return_code = 0U;
    if (output_size == 0U)
        return EDGE_IO_PROTOCOL_ERROR;
    if (!exchange(device, output, output_size, response, sizeof(response), &response_size))
        return EDGE_IO_NO_RESPONSE;
    const edge_s7_result result = edge_s7_parse_read(response, response_size, reference,
                                                     data, capacity, data_size, &return_code);
    if (result != EDGE_S7_OK)
        return EDGE_IO_PROTOCOL_ERROR;
    if (address.bit_access && *data_size != 0U) {
        data[0] = data[0] != 0U ? 1U : 0U;
        *data_size = 1U;
    }
    return EDGE_IO_OK;
}

static uint16_t be16(const uint8_t *value) {
    return (uint16_t)(((uint16_t)value[0] << 8U) | value[1]);
}

static uint32_t be32(const uint8_t *value) {
    return ((uint32_t)value[0] << 24U) | ((uint32_t)value[1] << 16U) |
           ((uint32_t)value[2] << 8U) | value[3];
}

static uint64_t be64(const uint8_t *value) {
    uint64_t output = 0U;
    for (size_t index = 0U; index < 8U; ++index)
        output = (output << 8U) | value[index];
    return output;
}

static void order_bytes(uint8_t *output, const uint8_t *input, size_t size,
                        const char *order) {
    memcpy(output, input, size);
    if (strcmp(order, "LITTLE_ENDIAN") == 0) {
        for (size_t left = 0U, right = size == 0U ? 0U : size - 1U; left < right;
             ++left, --right) {
            const uint8_t temporary = output[left];
            output[left] = output[right];
            output[right] = temporary;
        }
    } else if (strcmp(order, "BIG_ENDIAN_BYTE_SWAP") == 0) {
        for (size_t index = 0U; index + 1U < size; index += 2U) {
            const uint8_t temporary = output[index];
            output[index] = output[index + 1U];
            output[index + 1U] = temporary;
        }
    } else if (strcmp(order, "LITTLE_ENDIAN_BYTE_SWAP") == 0) {
        const size_t words = size / 2U;
        for (size_t left = 0U; left < words / 2U; ++left) {
            const size_t right = words - 1U - left;
            uint8_t temporary[2] = {output[left * 2U], output[left * 2U + 1U]};
            output[left * 2U] = output[right * 2U];
            output[left * 2U + 1U] = output[right * 2U + 1U];
            output[right * 2U] = temporary[0];
            output[right * 2U + 1U] = temporary[1];
        }
    }
}

static double rounded(double value, double scale, int32_t decimals) {
    value *= scale;
    if (decimals >= 0 && decimals <= 9) {
        double factor = 1.0;
        for (int32_t index = 0; index < decimals; ++index)
            factor *= 10.0;
        value = round(value * factor) / factor;
    }
    return value;
}

static void scalar_bool(iot_edge_v1_ScalarValue *value, bool input) {
    value->kind = iot_edge_v1_ValueKind_VALUE_BOOL;
    value->which_value = iot_edge_v1_ScalarValue_bool_value_tag;
    value->value.bool_value = input;
}

static void scalar_signed(iot_edge_v1_ScalarValue *value, int64_t input) {
    value->kind = iot_edge_v1_ValueKind_VALUE_SIGNED;
    value->which_value = iot_edge_v1_ScalarValue_signed_value_tag;
    value->value.signed_value = input;
}

static void scalar_unsigned(iot_edge_v1_ScalarValue *value, uint64_t input) {
    value->kind = iot_edge_v1_ValueKind_VALUE_UNSIGNED;
    value->which_value = iot_edge_v1_ScalarValue_unsigned_value_tag;
    value->value.unsigned_value = input;
}

static void scalar_double(iot_edge_v1_ScalarValue *value, double input) {
    value->kind = iot_edge_v1_ValueKind_VALUE_DOUBLE;
    value->which_value = iot_edge_v1_ScalarValue_double_value_tag;
    value->value.double_value = input;
}

static bool decode_scalar(const iot_edge_v1_ConfigItem *item, const uint8_t *raw,
                          size_t size, iot_edge_v1_ScalarValue *value) {
    const char *type = NULL;
    double scale = 1.0;
    int32_t decimals = -1;
    uint8_t ordered[EDGE_DEVICE_VALUE_MAX];
    if (item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag) {
        const iot_edge_v1_ModbusRegisterConfig *point = &item->item.modbus_register;
        type = point->data_type;
        scale = point->scale;
        decimals = point->decimals;
        order_bytes(ordered, raw, size, point->byte_order);
        raw = ordered;
    } else {
        const iot_edge_v1_S7AreaConfig *point = &item->item.s7_area;
        type = point->data_type;
        scale = point->scale;
        decimals = point->decimals;
    }
    *value = (iot_edge_v1_ScalarValue)iot_edge_v1_ScalarValue_init_zero;
    if (strcmp(type, "BOOL") == 0 && size >= 1U) {
        scalar_bool(value, raw[0] != 0U);
        return true;
    }
    if ((strcmp(type, "UINT8") == 0 || strcmp(type, "BYTE") == 0) && size >= 1U) {
        scalar_unsigned(value, raw[0]);
        return true;
    }
    if (strcmp(type, "INT8") == 0 && size >= 1U) {
        scalar_signed(value, (int8_t)raw[0]);
        return true;
    }
    uint64_t numeric = size >= 8U ? be64(raw) : size >= 4U ? be32(raw)
                               : size >= 2U ? be16(raw) : 0U;
    if (strcmp(type, "INT16") == 0 && size >= 2U) {
        if (scale != 1.0 || decimals >= 0)
            scalar_double(value, rounded((int16_t)numeric, scale, decimals));
        else
            scalar_signed(value, (int16_t)numeric);
        return true;
    }
    if ((strcmp(type, "UINT16") == 0 || strcmp(type, "WORD") == 0) && size >= 2U) {
        if (scale != 1.0 || decimals >= 0)
            scalar_double(value, rounded((uint16_t)numeric, scale, decimals));
        else
            scalar_unsigned(value, (uint16_t)numeric);
        return true;
    }
    if (strcmp(type, "INT32") == 0 && size >= 4U) {
        if (scale != 1.0 || decimals >= 0)
            scalar_double(value, rounded((int32_t)numeric, scale, decimals));
        else
            scalar_signed(value, (int32_t)numeric);
        return true;
    }
    if ((strcmp(type, "UINT32") == 0 || strcmp(type, "DWORD") == 0) && size >= 4U) {
        if (scale != 1.0 || decimals >= 0)
            scalar_double(value, rounded((uint32_t)numeric, scale, decimals));
        else
            scalar_unsigned(value, (uint32_t)numeric);
        return true;
    }
    if (strcmp(type, "INT64") == 0 && size >= 8U) {
        scalar_signed(value, (int64_t)numeric);
        return true;
    }
    if (strcmp(type, "UINT64") == 0 && size >= 8U) {
        scalar_unsigned(value, numeric);
        return true;
    }
    if ((strcmp(type, "FLOAT") == 0 || strcmp(type, "FLOAT32") == 0 ||
         strcmp(type, "REAL") == 0) && size >= 4U) {
        const uint32_t bits = (uint32_t)numeric;
        float parsed;
        memcpy(&parsed, &bits, sizeof(parsed));
        scalar_double(value, rounded(parsed, scale, decimals));
        return isfinite(value->value.double_value);
    }
    if ((strcmp(type, "DOUBLE") == 0 || strcmp(type, "LREAL") == 0) && size >= 8U) {
        double parsed;
        memcpy(&parsed, &numeric, sizeof(parsed));
        scalar_double(value, rounded(parsed, scale, decimals));
        return isfinite(value->value.double_value);
    }
    if (strcmp(type, "STRING") == 0 && size != 0U) {
        value->kind = iot_edge_v1_ValueKind_VALUE_STRING;
        value->which_value = iot_edge_v1_ScalarValue_string_value_tag;
        const size_t copy = size < sizeof(value->value.string_value) - 1U
                                ? size : sizeof(value->value.string_value) - 1U;
        memcpy(value->value.string_value, raw, copy);
        value->value.string_value[copy] = '\0';
        return true;
    }
    return false;
}

static void fill_point_value(edge_acquisition_point *point, const uint8_t *raw, size_t size) {
    point->value = (iot_edge_v1_TelemetryValue)iot_edge_v1_TelemetryValue_init_zero;
    if (point->item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag) {
        const iot_edge_v1_ModbusRegisterConfig *config = &point->item->item.modbus_register;
        copy_text(point->value.element_id, sizeof(point->value.element_id), config->element_id);
        copy_text(point->value.name, sizeof(point->value.name), config->name);
        copy_text(point->value.unit, sizeof(point->value.unit), config->unit);
    } else {
        const iot_edge_v1_S7AreaConfig *config = &point->item->item.s7_area;
        copy_text(point->value.element_id, sizeof(point->value.element_id), config->element_id);
        copy_text(point->value.name, sizeof(point->value.name), config->name);
        copy_text(point->value.unit, sizeof(point->value.unit), config->unit);
    }
    point->value.has_value = decode_scalar(point->item, raw, size, &point->value.value);
    point->valid = point->value.has_value;
}

static edge_io_result device_read(void *context, edge_device_sample *sample) {
    edge_acquisition_device *device = context;
    bool any = false;
    for (size_t index = 0U; index < device->point_count; ++index) {
        edge_acquisition_point *point = &device->points[index];
        point->valid = false;
        uint8_t raw[EDGE_DEVICE_VALUE_MAX];
        size_t raw_size = 0U;
        edge_io_result result = point->item->which_item ==
                                        iot_edge_v1_ConfigItem_modbus_register_tag
                                    ? read_modbus_point(device,
                                          &point->item->item.modbus_register,
                                          raw, sizeof(raw), &raw_size)
                                    : read_s7_point(device, &point->item->item.s7_area,
                                          raw, sizeof(raw), &raw_size);
        if (result != EDGE_IO_OK)
            return result;
        fill_point_value(point, raw, raw_size);
        any = any || point->valid;
    }
    sample->bytes[0] = any || device->point_count == 0U ? 1U : 0U;
    sample->size = 1U;
    return EDGE_IO_OK;
}

static edge_acquisition_point *find_point(edge_acquisition_device *device,
                                           const char *element_id) {
    for (size_t index = 0U; index < device->point_count; ++index) {
        const iot_edge_v1_ConfigItem *item = device->points[index].item;
        const char *current = item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag
                                  ? item->item.modbus_register.element_id
                                  : item->item.s7_area.element_id;
        if (strcmp(current, element_id) == 0)
            return &device->points[index];
    }
    return NULL;
}

static bool parse_signed(const char *text, int64_t *output) {
    char *end = NULL;
    errno = 0;
    const long long value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    *output = (int64_t)value;
    return true;
}

static bool parse_unsigned(const char *text, uint64_t *output) {
    char *end = NULL;
    errno = 0;
    const unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0')
        return false;
    *output = (uint64_t)value;
    return true;
}

static bool parse_double_value(const char *text, double *output) {
    char *end = NULL;
    errno = 0;
    const double value = strtod(text, &end);
    if (errno != 0 || end == text || *end != '\0' || !isfinite(value))
        return false;
    *output = value;
    return true;
}

static void put_be(uint8_t *output, uint64_t value, size_t size) {
    for (size_t index = 0U; index < size; ++index)
        output[index] = (uint8_t)(value >> ((size - 1U - index) * 8U));
}

static bool encode_scalar(const iot_edge_v1_ConfigItem *item, const char *text,
                          uint8_t *output, size_t capacity, size_t *size) {
    const char *type;
    const char *order = "BIG_ENDIAN";
    size_t width = 0U;
    if (item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag) {
        type = item->item.modbus_register.data_type;
        order = item->item.modbus_register.byte_order;
        width = (size_t)item->item.modbus_register.quantity * 2U;
        if (strcmp(type, "BOOL") == 0)
            width = 1U;
    } else {
        type = item->item.s7_area.data_type;
        width = item->item.s7_area.size;
        if (strcmp(type, "BOOL") == 0)
            width = 1U;
    }
    if (width == 0U || width > capacity)
        return false;
    uint8_t canonical[EDGE_DEVICE_VALUE_MAX] = {0};
    int64_t signed_value = 0;
    uint64_t unsigned_value = 0U;
    double decimal = 0.0;
    if (strcmp(type, "BOOL") == 0) {
        if (strcmp(text, "0") != 0 && strcmp(text, "1") != 0)
            return false;
        canonical[0] = text[0] == '1' ? 1U : 0U;
    } else if (strcmp(type, "INT8") == 0 || strcmp(type, "INT16") == 0 ||
               strcmp(type, "INT32") == 0 || strcmp(type, "INT64") == 0) {
        if (!parse_signed(text, &signed_value))
            return false;
        put_be(canonical, (uint64_t)signed_value, width);
    } else if (strcmp(type, "UINT8") == 0 || strcmp(type, "BYTE") == 0 ||
               strcmp(type, "UINT16") == 0 || strcmp(type, "WORD") == 0 ||
               strcmp(type, "UINT32") == 0 || strcmp(type, "DWORD") == 0 ||
               strcmp(type, "UINT64") == 0) {
        if (!parse_unsigned(text, &unsigned_value))
            return false;
        put_be(canonical, unsigned_value, width);
    } else if (strcmp(type, "FLOAT") == 0 || strcmp(type, "FLOAT32") == 0 ||
               strcmp(type, "REAL") == 0) {
        if (width != 4U || !parse_double_value(text, &decimal))
            return false;
        const float narrowed = (float)decimal;
        uint32_t bits;
        memcpy(&bits, &narrowed, sizeof(bits));
        put_be(canonical, bits, width);
    } else if (strcmp(type, "DOUBLE") == 0 || strcmp(type, "LREAL") == 0) {
        if (width != 8U || !parse_double_value(text, &decimal))
            return false;
        uint64_t bits;
        memcpy(&bits, &decimal, sizeof(bits));
        put_be(canonical, bits, width);
    } else if (strcmp(type, "STRING") == 0) {
        const size_t length = strlen(text);
        if (length > width)
            return false;
        memcpy(canonical, text, length);
    } else {
        return false;
    }
    order_bytes(output, canonical, width, order);
    *size = width;
    return true;
}

static edge_io_result write_modbus(edge_acquisition_device *device,
                                    const iot_edge_v1_ModbusRegisterConfig *point,
                                    const edge_write_command *command,
                                    edge_device_sample *actual) {
    const uint8_t read_function = modbus_function(point->register_type);
    edge_modbus_request request = {
        .transport = modbus_transport(device),
        .transaction_id = ++device->transaction,
        .unit_id = (uint8_t)device->config->modbus_slave_id,
        .function = read_function == 1U ? 5U : point->quantity == 1U ? 6U : 16U,
        .address = (uint16_t)point->address,
        .quantity = (uint16_t)point->quantity};
    uint8_t output[EDGE_MODBUS_MAX_FRAME];
    uint8_t response[EDGE_MODBUS_MAX_FRAME];
    size_t output_size = 0U;
    size_t response_size = 0U;
    size_t ignored = 0U;
    uint8_t exception = 0U;
    if (edge_modbus_build_write(&request, command->value, command->value_size,
                                output, sizeof(output), &output_size) != EDGE_MODBUS_OK)
        return EDGE_IO_PROTOCOL_ERROR;
    if (!exchange(device, output, output_size, response, sizeof(response), &response_size))
        return EDGE_IO_NO_RESPONSE;
    if (edge_modbus_parse_response(&request, response, response_size, command->value,
                                   command->value_size, NULL, 0U, &ignored,
                                   &exception) != EDGE_MODBUS_OK)
        return EDGE_IO_PROTOCOL_ERROR;
    return read_modbus_point(device, point, actual->bytes, sizeof(actual->bytes),
                              &actual->size);
}

static edge_io_result write_s7(edge_acquisition_device *device,
                                const iot_edge_v1_S7AreaConfig *point,
                                const edge_write_command *command,
                                edge_device_sample *actual) {
    const edge_s7_address address = s7_address(point);
    const uint16_t reference = ++device->s7_reference;
    uint8_t output[EDGE_S7_MAX_FRAME];
    uint8_t response[EDGE_S7_MAX_FRAME];
    const size_t output_size = edge_s7_build_write(reference, &address, command->value,
                                                   command->value_size, output,
                                                   sizeof(output));
    size_t response_size = 0U;
    uint8_t return_code = 0U;
    if (output_size == 0U)
        return EDGE_IO_PROTOCOL_ERROR;
    if (!exchange(device, output, output_size, response, sizeof(response), &response_size))
        return EDGE_IO_NO_RESPONSE;
    if (edge_s7_parse_write(response, response_size, reference, &return_code) != EDGE_S7_OK)
        return EDGE_IO_PROTOCOL_ERROR;
    return read_s7_point(device, point, actual->bytes, sizeof(actual->bytes), &actual->size);
}

static edge_io_result device_write_readback(void *context,
                                             const edge_write_command *command,
                                             edge_device_sample *actual) {
    edge_acquisition_device *device = context;
    edge_acquisition_point *point = find_point(device, command->element_id);
    if (point == NULL)
        return EDGE_IO_PROTOCOL_ERROR;
    const edge_io_result result = point->item->which_item ==
                                          iot_edge_v1_ConfigItem_modbus_register_tag
                                      ? write_modbus(device,
                                            &point->item->item.modbus_register,
                                            command, actual)
                                      : write_s7(device, &point->item->item.s7_area,
                                            command, actual);
    if (result == EDGE_IO_OK)
        fill_point_value(point, actual->bytes, actual->size);
    return result;
}

static void device_report(void *context, const uint8_t platform_id[16],
                          const uint8_t device_id[16],
                          const edge_device_sample *sample) {
    edge_acquisition_device *device = context;
    (void)platform_id;
    (void)device_id;
    device->observed_at_ms = sample->sampled_at_ms;
    size_t index = 0U;
    while (index < device->point_count) {
        iot_edge_v1_TelemetryRecord record = iot_edge_v1_TelemetryRecord_init_zero;
        uint8_t id[16];
        record_id((uint64_t)device->observed_at_ms, id);
        edge_protocol_set_bytes(&record.record_id, sizeof(record.record_id.bytes), id, 16U);
        edge_protocol_set_bytes(&record.device_id, sizeof(record.device_id.bytes),
                                device->config->device_id.bytes,
                                device->config->device_id.size);
        edge_protocol_set_bytes(&record.endpoint_id, sizeof(record.endpoint_id.bytes),
                                device->config->endpoint_id.bytes,
                                device->config->endpoint_id.size);
        record.protocol = device->config->protocol;
        copy_text(record.function_code, sizeof(record.function_code),
                  device->config->protocol == iot_edge_v1_Protocol_PROTOCOL_MODBUS
                      ? "POLL" : "READ");
        copy_text(record.function_name, sizeof(record.function_name), "定时采集");
        copy_text(record.direction, sizeof(record.direction), "UP");
        record.observed_at_ms = device->observed_at_ms;
        while (index < device->point_count && record.values_count < 16U) {
            if (device->points[index].valid)
                record.values[record.values_count++] = device->points[index].value;
            ++index;
        }
        if (record.values_count != 0U)
            (void)device->owner->telemetry(device->owner->callback_context, &record);
    }
}

static iot_edge_v1_CommandState command_state(edge_command_result result) {
    switch (result) {
    case EDGE_COMMAND_SUCCEEDED:
        return iot_edge_v1_CommandState_COMMAND_STATE_SUCCEEDED;
    case EDGE_COMMAND_READBACK_MISMATCH:
        return iot_edge_v1_CommandState_COMMAND_STATE_READBACK_MISMATCH;
    case EDGE_COMMAND_DEVICE_OFFLINE:
        return iot_edge_v1_CommandState_COMMAND_STATE_DEVICE_OFFLINE;
    case EDGE_COMMAND_TIMED_OUT:
        return iot_edge_v1_CommandState_COMMAND_STATE_TIMED_OUT;
    default:
        return iot_edge_v1_CommandState_COMMAND_STATE_FAILED;
    }
}

static void device_command_complete(void *context, const uint8_t platform_id[16],
                                    const uint8_t device_id[16],
                                    const uint8_t command_id[16],
                                    edge_command_result result,
                                    const edge_device_sample *actual) {
    edge_acquisition_device *device = context;
    (void)platform_id;
    (void)device_id;
    iot_edge_v1_CommandResult output = iot_edge_v1_CommandResult_init_zero;
    edge_protocol_set_bytes(&output.command_id, sizeof(output.command_id.bytes),
                            command_id, 16U);
    edge_protocol_set_bytes(&output.device_id, sizeof(output.device_id.bytes),
                            device->config->device_id.bytes,
                            device->config->device_id.size);
    output.state = command_state(result);
    output.completed_at_ms = current_ms();
    copy_text(output.message, sizeof(output.message),
              result == EDGE_COMMAND_SUCCEEDED ? "write and readback succeeded"
              : result == EDGE_COMMAND_READBACK_MISMATCH ? "write readback mismatch"
              : result == EDGE_COMMAND_DEVICE_OFFLINE ? "device offline"
              : result == EDGE_COMMAND_TIMED_OUT ? "device response timed out"
                                                 : "device command failed");
    const edge_write_command *command = &device->runtime.writes[device->runtime.write_head];
    edge_acquisition_point *point = find_point(device, command->element_id);
    if (point != NULL && actual != NULL && actual->size != 0U) {
        fill_point_value(point, actual->bytes, actual->size);
        if (point->valid) {
            output.actual_values[0] = point->value;
            output.actual_values_count = 1U;
        }
    }
    (void)device->owner->command(device->owner->callback_context, &output);
}

static const edge_device_driver kDriver = {
    .connect = device_connect,
    .handshake = device_handshake,
    .read = device_read,
    .write_readback = device_write_readback,
    .disconnect = device_disconnect,
    .report = device_report,
    .command_complete = device_command_complete};

static void free_devices(edge_acquisition_device *devices, size_t count) {
    if (devices == NULL)
        return;
    for (size_t index = 0U; index < count; ++index) {
        edge_device_runtime_close(&devices[index].runtime);
        close_fd(&devices[index].listen_fd);
        free(devices[index].points);
    }
    free(devices);
}

edge_acquisition *edge_acquisition_create(
    edge_acquisition_telemetry_callback telemetry,
    edge_acquisition_command_callback command, void *callback_context) {
    if (telemetry == NULL || command == NULL)
        return NULL;
    edge_acquisition *value = calloc(1U, sizeof(*value));
    if (value != NULL) {
        value->telemetry = telemetry;
        value->command = command;
        value->callback_context = callback_context;
    }
    return value;
}

static bool point_for_device(const iot_edge_v1_ConfigItem *item,
                             const iot_edge_v1_DeviceConfig *device) {
    if (device->protocol == iot_edge_v1_Protocol_PROTOCOL_MODBUS)
        return item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag &&
               same_id(&item->item.modbus_register.device_id, device->device_id.bytes);
    return item->which_item == iot_edge_v1_ConfigItem_s7_area_tag &&
           same_id(&item->item.s7_area.device_id, device->device_id.bytes);
}

bool edge_acquisition_apply(edge_acquisition *acquisition,
                            const edge_runtime_config *config,
                            uint64_t now_ms, char *error, size_t error_size) {
    if (acquisition == NULL || config == NULL) {
        set_error(error, error_size, "invalid acquisition configuration");
        return false;
    }
    size_t enabled = 0U;
    for (uint32_t index = 0U; index < config->item_count; ++index)
        if (config->items[index].which_item == iot_edge_v1_ConfigItem_device_tag &&
            config->items[index].item.device.enabled)
            ++enabled;
    edge_acquisition_device *devices = enabled != 0U ? calloc(enabled, sizeof(*devices)) : NULL;
    if (enabled != 0U && devices == NULL) {
        set_error(error, error_size, "acquisition memory allocation failed");
        return false;
    }
    for (size_t index = 0U; index < enabled; ++index) {
        devices[index].fd = -1;
        devices[index].listen_fd = -1;
    }
    size_t output = 0U;
    for (uint32_t index = 0U; index < config->item_count; ++index) {
        const iot_edge_v1_ConfigItem *item = &config->items[index];
        if (item->which_item != iot_edge_v1_ConfigItem_device_tag || !item->item.device.enabled)
            continue;
        const iot_edge_v1_DeviceConfig *device = &item->item.device;
        const iot_edge_v1_EndpointConfig *endpoint =
            edge_runtime_config_endpoint(config, device->endpoint_id.bytes);
        if (endpoint == NULL || !endpoint->enabled ||
            (device->protocol != iot_edge_v1_Protocol_PROTOCOL_MODBUS &&
             device->protocol != iot_edge_v1_Protocol_PROTOCOL_S7)) {
            free_devices(devices, enabled);
            set_error(error, error_size, "only enabled Modbus and S7 endpoints are supported");
            return false;
        }
        if (device->protocol == iot_edge_v1_Protocol_PROTOCOL_S7 &&
            (endpoint->transport != iot_edge_v1_Transport_TRANSPORT_ETHERNET ||
             endpoint->mode != iot_edge_v1_LinkMode_LINK_MODE_TCP_CLIENT)) {
            free_devices(devices, enabled);
            set_error(error, error_size, "S7 requires TCP Client endpoint");
            return false;
        }
        edge_acquisition_device *runtime = &devices[output++];
        runtime->owner = acquisition;
        runtime->endpoint = endpoint;
        runtime->config = device;
        runtime->fd = -1;
        runtime->listen_fd = -1;
        for (uint32_t point = 0U; point < config->item_count; ++point)
            if (point_for_device(&config->items[point], device))
                ++runtime->point_count;
        if (runtime->point_count != 0U) {
            runtime->points = calloc(runtime->point_count, sizeof(*runtime->points));
            if (runtime->points == NULL) {
                free_devices(devices, enabled);
                set_error(error, error_size, "point memory allocation failed");
                return false;
            }
            size_t point_output = 0U;
            for (uint32_t point = 0U; point < config->item_count; ++point)
                if (point_for_device(&config->items[point], device))
                    runtime->points[point_output++].item = &config->items[point];
        }
        uint8_t platform[16] = {0};
        const edge_device_protocol protocol =
            device->protocol == iot_edge_v1_Protocol_PROTOCOL_MODBUS
                ? EDGE_DEVICE_MODBUS : EDGE_DEVICE_S7;
        if (!edge_device_runtime_init(&runtime->runtime, protocol, platform,
                                      device->device_id.bytes, device->io_interval_ms,
                                      device->report_interval_sec, now_ms,
                                      &kDriver, runtime)) {
            free_devices(devices, enabled);
            set_error(error, error_size, "device runtime initialization failed");
            return false;
        }
    }
    free_devices(acquisition->devices, acquisition->device_count);
    acquisition->devices = devices;
    acquisition->device_count = enabled;
    return true;
}

void edge_acquisition_tick(edge_acquisition *acquisition, uint64_t now_ms) {
    if (acquisition == NULL)
        return;
    for (size_t index = 0U; index < acquisition->device_count; ++index)
        edge_device_runtime_tick(&acquisition->devices[index].runtime, now_ms);
}

static edge_acquisition_device *find_device(edge_acquisition *acquisition,
                                             const uint8_t id[16]) {
    for (size_t index = 0U; index < acquisition->device_count; ++index)
        if (same_id(&acquisition->devices[index].config->device_id, id))
            return &acquisition->devices[index];
    return NULL;
}

static bool writable_point(const edge_acquisition_point *point) {
    return point->item->which_item == iot_edge_v1_ConfigItem_modbus_register_tag
               ? point->item->item.modbus_register.writable
               : point->item->item.s7_area.writable;
}

bool edge_acquisition_command(edge_acquisition *acquisition,
                              const iot_edge_v1_CommandRequest *request,
                              char *error, size_t error_size) {
    if (acquisition == NULL || request == NULL || request->command_id.size != 16U ||
        request->device_id.size != 16U || request->values_count != 1U) {
        set_error(error, error_size, "edge command requires one device and one value");
        return false;
    }
    edge_acquisition_device *device = find_device(acquisition, request->device_id.bytes);
    if (device == NULL) {
        set_error(error, error_size, "edge device is not active in this platform config");
        return false;
    }
    const iot_edge_v1_CommandValue *value = &request->values[0];
    edge_acquisition_point *point = find_point(device, value->element_id);
    if (point == NULL || !writable_point(point)) {
        set_error(error, error_size, "command element is missing or not writable");
        return false;
    }
    if (!value->has_expected ||
        value->expected.which_value != iot_edge_v1_ScalarValue_string_value_tag) {
        set_error(error, error_size, "command value must be a string scalar");
        return false;
    }
    edge_write_command command;
    memset(&command, 0, sizeof(command));
    memcpy(command.command_id, request->command_id.bytes, 16U);
    copy_text(command.element_id, sizeof(command.element_id), value->element_id);
    if (!encode_scalar(point->item, value->expected.value.string_value,
                       command.value, sizeof(command.value), &command.value_size)) {
        set_error(error, error_size, "command value cannot be encoded for the configured type");
        return false;
    }
    if (!edge_device_runtime_enqueue_write(&device->runtime, &command)) {
        set_error(error, error_size, "device write queue is full");
        return false;
    }
    return true;
}

void edge_acquisition_destroy(edge_acquisition *acquisition) {
    if (acquisition == NULL)
        return;
    free_devices(acquisition->devices, acquisition->device_count);
    free(acquisition);
}
