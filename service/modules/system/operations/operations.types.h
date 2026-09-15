#pragma once

#include <string>

namespace service::system {

    struct OperationsReadiness {
        bool ready;
        std::string json;
    };

    struct WorkerDiagnosticSnapshot final {
        bool metricsPresent{ false };
        bool readinessPresent{ false };
        bool ready{ false };
        std::string metrics;
        std::string healthJson;
    };

} // namespace service::system
