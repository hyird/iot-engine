#include "edge_capability.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <glob.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool edge_capability_has_ttyd(void) {
    static const char *paths[] = {"/usr/bin/ttyd", "/usr/sbin/ttyd", "/bin/ttyd"};
    for (size_t index = 0; index < sizeof(paths) / sizeof(paths[0]); ++index)
        if (access(paths[index], X_OK) == 0)
            return true;
    return false;
}

static void copy_text(char *output, size_t capacity, const char *input) {
    if (capacity != 0U)
        snprintf(output, capacity, "%s", input != NULL ? input : "");
}

static iot_edge_v1_InterfaceCapability *find_interface(
    iot_edge_v1_CapabilityReport *report, const char *name) {
    for (pb_size_t index = 0; index < report->interfaces_count; ++index) {
        if (strcmp(report->interfaces[index].name, name) == 0)
            return &report->interfaces[index];
    }
    if (report->interfaces_count >= 8U)
        return NULL;
    iot_edge_v1_InterfaceCapability *item =
        &report->interfaces[report->interfaces_count++];
    memset(item, 0, sizeof(*item));
    copy_text(item->name, sizeof(item->name), name);
    copy_text(item->display_name, sizeof(item->display_name), name);
    return item;
}

static uint32_t prefix_length(const struct sockaddr *address) {
    if (address == NULL || address->sa_family != AF_INET)
        return 0U;
    uint32_t mask = ntohl(((const struct sockaddr_in *)address)->sin_addr.s_addr);
    uint32_t prefix = 0U;
    while ((mask & 0x80000000U) != 0U) {
        ++prefix;
        mask <<= 1U;
    }
    return mask == 0U ? prefix : 0U;
}

static void collect_bridge_ports(iot_edge_v1_InterfaceCapability *item) {
    char path[128];
    snprintf(path, sizeof(path), "/sys/class/net/%s/brif", item->name);
    DIR *directory = opendir(path);
    if (directory == NULL)
        return;
    item->bridge = true;
    struct dirent *entry;
    while (item->bridge_ports_count < 8U && (entry = readdir(directory)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;
        copy_text(item->bridge_ports[item->bridge_ports_count],
                  sizeof(item->bridge_ports[item->bridge_ports_count]), entry->d_name);
        ++item->bridge_ports_count;
    }
    closedir(directory);
}

static void collect_interfaces(iot_edge_v1_CapabilityReport *report) {
    struct ifaddrs *addresses = NULL;
    if (getifaddrs(&addresses) != 0)
        return;
    for (const struct ifaddrs *entry = addresses; entry != NULL; entry = entry->ifa_next) {
        if (entry->ifa_name == NULL || entry->ifa_addr == NULL)
            continue;
        const int family = entry->ifa_addr->sa_family;
        if (family != AF_INET && family != AF_PACKET)
            continue;
        iot_edge_v1_InterfaceCapability *item = find_interface(report, entry->ifa_name);
        if (item == NULL)
            continue;
        item->up = (entry->ifa_flags & IFF_UP) != 0U;
        if (family == AF_INET) {
            const struct sockaddr_in *ipv4 = (const struct sockaddr_in *)entry->ifa_addr;
            (void)inet_ntop(AF_INET, &ipv4->sin_addr, item->ipv4, sizeof(item->ipv4));
            item->prefix_length = prefix_length(entry->ifa_netmask);
        } else {
            const struct sockaddr_ll *link = (const struct sockaddr_ll *)entry->ifa_addr;
            if (link->sll_halen == 6U) {
                item->mac.size = 6U;
                memcpy(item->mac.bytes, link->sll_addr, 6U);
            }
        }
    }
    freeifaddrs(addresses);
    for (pb_size_t index = 0; index < report->interfaces_count; ++index)
        collect_bridge_ports(&report->interfaces[index]);
}

static bool serial_seen(const iot_edge_v1_CapabilityReport *report, const char *path) {
    for (pb_size_t index = 0; index < report->serial_ports_count; ++index) {
        if (strcmp(report->serial_ports[index].path, path) == 0)
            return true;
    }
    return false;
}

static void collect_serial(iot_edge_v1_CapabilityReport *report) {
    static const char *patterns[] = {
        "/dev/ttyS*", "/dev/ttyUSB*", "/dev/ttyACM*", "/dev/ttyATH*"};
    glob_t matches;
    memset(&matches, 0, sizeof(matches));
    int flags = 0;
    for (size_t index = 0; index < sizeof(patterns) / sizeof(patterns[0]); ++index) {
        const int result = glob(patterns[index], flags, NULL, &matches);
        if (result != 0 && result != GLOB_NOMATCH)
            break;
        flags = GLOB_APPEND;
    }
    for (size_t index = 0; index < matches.gl_pathc && report->serial_ports_count < 8U;
         ++index) {
        const char *path = matches.gl_pathv[index];
        if (serial_seen(report, path))
            continue;
        iot_edge_v1_SerialCapability *item =
            &report->serial_ports[report->serial_ports_count++];
        memset(item, 0, sizeof(*item));
        copy_text(item->path, sizeof(item->path), path);
        const char *name = strrchr(path, '/');
        copy_text(item->display_name, sizeof(item->display_name), name != NULL ? name + 1 : path);
        item->available = access(path, R_OK | W_OK) == 0;
        /* RS485 capability cannot be inferred reliably from a tty name alone. */
        item->rs485 = false;
    }
    globfree(&matches);
}

void edge_capability_collect(iot_edge_v1_CapabilityReport *report) {
    if (report == NULL)
        return;
    memset(report, 0, sizeof(*report));
    copy_text(report->network_backend, sizeof(report->network_backend), "uci");
    report->ttyd_available = edge_capability_has_ttyd();
    collect_interfaces(report);
    collect_serial(report);
}
