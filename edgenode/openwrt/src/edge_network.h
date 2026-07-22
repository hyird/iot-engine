#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "edge.pb.h"

bool edge_network_validate_static(const char *ip, uint32_t prefix_length,
                                  const char *gateway, char *error, size_t error_size);

/* Stages and commits br-lan exclusively through uci commands; it does not reload network. */
bool edge_network_prepare_br_lan(const iot_edge_v1_NetworkConfigRequest *request,
                                 char *error, size_t error_size);

/* Reloads network and starts a rollback watchdog. */
bool edge_network_activate(uint32_t rollback_timeout_sec);

/* Called only after the network-owner platform reconnects successfully. */
void edge_network_confirm(void);
