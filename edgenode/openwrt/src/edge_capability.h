#pragma once

#include "edge.pb.h"

/* Collects the node's current Ethernet/bridge and serial-port inventory. */
void edge_capability_collect(iot_edge_v1_CapabilityReport *report);
bool edge_capability_has_ttyd(void);
