#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "edge.pb.h"

/* Applies a non-bootstrap platform definition exclusively through UCI commands. */
bool edge_platform_apply(const iot_edge_v1_PlatformConfigRequest *request,
                         char *error, size_t error_size);
