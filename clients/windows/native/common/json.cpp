#include "json.h"
#include "win32.h"
#include <sddl.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

namespace iotvpn {
Json parseJson(std::string_view text) {
    if (text.size() > MaxMessageBytes) throw std::runtime_error("Message exceeds 1 MiB");
    return Json::parse(text, [](int depth, Json::parse_event_t, Json&) {
        if (depth > 16) throw std::runtime_error("JSON nesting exceeds limit");
        return true;
    });
}


}
