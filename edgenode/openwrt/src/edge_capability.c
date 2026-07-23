#include "edge_capability.h"

#include <stddef.h>
#include <unistd.h>

bool edge_capability_has_ttyd(void) {
    static const char *paths[] = {"/usr/bin/ttyd", "/usr/sbin/ttyd", "/bin/ttyd"};
    for (size_t index = 0; index < sizeof(paths) / sizeof(paths[0]); ++index)
        if (access(paths[index], X_OK) == 0)
            return true;
    return false;
}
