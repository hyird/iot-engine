#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "service/config/lifecycle.h"
#include "service/common/message.h"

namespace {

void require(bool condition, std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

std::filesystem::path repositoryRoot() {
    return std::filesystem::path(__FILE__).parent_path().parent_path();
}

std::string source(std::string_view relative) {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      std::filesystem::path(relative);
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot read architecture source");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void requireFeatureRule(bool condition, const std::filesystem::path& path,
                        std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message) + ": " + path.generic_string());
}

bool isSnakeCase(std::string_view value) {
    if (value.empty() || value.front() < 'a' || value.front() > 'z' ||
        value.back() == '_')
        return false;
    bool previousUnderscore = false;
    for (const auto character : value) {
        if (character == '_') {
            if (previousUnderscore)
                return false;
            previousUnderscore = true;
            continue;
        }
        if ((character < 'a' || character > 'z') &&
            (character < '0' || character > '9'))
            return false;
        previousUnderscore = false;
    }
    return true;
}

template <std::size_t Size>
bool contains(const std::array<std::string_view, Size>& values, std::string_view value) {
    for (const auto candidate : values)
        if (candidate == value)
            return true;
    return false;
}

bool isArchitectureSource(const std::filesystem::path& path) {
    static constexpr std::array<std::string_view, 7> extensions{
        ".h", ".hpp", ".hh", ".hxx", ".cpp", ".cc", ".cxx",
    };
    return contains(extensions, path.extension().string());
}

std::string pathKey(const std::filesystem::path& path) {
    return path.lexically_relative(repositoryRoot()).generic_string();
}

void requireExpectedModule(std::string_view domain, std::string_view module,
                           const std::filesystem::path& path) {
    static constexpr std::array<std::string_view, 10> moduleRoots{
        "system", "device", "link", "protocol", "alert", "gb28181",
        "command", "vpn", "open_access", "edge_node",
    };
    static constexpr std::array<std::string_view, 6> systemModules{
        "auth", "dept", "role", "user", "operations", "outbox",
    };
    static constexpr std::array<std::string_view, 9> rootModules{
        "device", "link", "protocol", "alert", "gb28181", "command",
        "vpn", "open_access", "edge_node",
    };
    requireFeatureRule(contains(moduleRoots, domain), path,
                       "module directory is not whitelisted");
    if (domain == "system") {
        requireFeatureRule(contains(systemModules, module), path,
                           "module name is not whitelisted for its domain");
    } else {
        requireFeatureRule(module.empty() && contains(rootModules, domain), path,
                           "root module may not contain nested directories");
    }
}

bool whitelistedModuleFile(std::string_view file, std::string_view owner) {
    static constexpr std::array<std::string_view, 6> suffixes{
        ".types.h", ".schema.h", ".service.h", ".entity.h", ".error.h",
        ".controller.h",
    };
    for (const auto suffix : suffixes) {
        if (file == std::string(owner) + std::string(suffix))
            return true;
    }
    return false;
}

void testServiceTopLevelLayout() {
    const auto root = repositoryRoot() / "service";
    static constexpr std::array<std::string_view, 7> allowed{
        "common", "config", "features", "middleware", "modules", "utils", "server.cpp",
    };
    require(std::filesystem::is_directory(root), "service directory is missing");
    for (const auto& entry : std::filesystem::directory_iterator(root))
        requireFeatureRule(contains(allowed, entry.path().filename().string()), entry.path(),
                           "service top-level entry is not whitelisted");
    for (const auto requiredDirectory : {"common", "config", "features", "middleware",
                                         "modules", "utils"})
        requireFeatureRule(std::filesystem::is_directory(root / requiredDirectory),
                           root / requiredDirectory,
                           "required service top-level directory is missing");
    requireFeatureRule(std::filesystem::is_regular_file(root / "server.cpp"),
                       root / "server.cpp", "service/server.cpp is missing");
}

bool whitelistedFeatureFile(std::string_view file, std::string_view owner) {
    static constexpr std::array<std::string_view, 12> suffixes{
        ".runtime.h",   ".runtime.cpp",  ".service.h",   ".types.h",
        ".entity.h",    ".config.h",     ".protocol.h",  ".protocol.cpp",
        ".transport.h", ".transport.cpp", ".error.h",     ".proto",
    };
    for (const auto suffix : suffixes) {
        if (file == std::string(owner) + std::string(suffix))
            return true;
    }
    return false;
}

std::vector<std::filesystem::path> featureFiles() {
    const auto root = repositoryRoot() / "service" / "features";
    require(std::filesystem::is_directory(root), "service/features directory is missing");
    std::vector<std::filesystem::path> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file())
            result.push_back(entry.path());
    }
    return result;
}

void testModuleFileLayout() {
    const auto root = repositoryRoot() / "service" / "modules";
    require(std::filesystem::is_directory(root), "service/modules directory is missing");
    const auto validateModuleFiles = [](const std::filesystem::path& modulePath,
                                        std::string_view moduleName) {
        for (const auto& entry : std::filesystem::directory_iterator(modulePath)) {
            requireFeatureRule(entry.is_regular_file(), entry.path(),
                               "module files may not contain nested directories");
            const auto file = entry.path().filename().string();
            requireFeatureRule(file.starts_with(std::string(moduleName) + "."), entry.path(),
                               "module file name must use its module prefix");
            requireFeatureRule(whitelistedModuleFile(file, moduleName), entry.path(),
                               "module file name or extension is not whitelisted");
        }
    };

    static constexpr std::array<std::string_view, 6> systemModules{
        "auth", "dept", "role", "user", "operations", "outbox",
    };
    static constexpr std::array<std::string_view, 9> rootModules{
        "device", "link", "protocol", "alert", "gb28181", "command", "vpn",
        "open_access", "edge_node",
    };
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        requireFeatureRule(entry.is_directory(), entry.path(),
                           "module entries must be directories");
        const auto entryName = entry.path().filename().string();
        requireFeatureRule(isSnakeCase(entryName), entry.path(),
                           "module directory must use snake_case");
        if (entryName == "system") {
            for (const auto& module : std::filesystem::directory_iterator(entry.path())) {
                requireFeatureRule(module.is_directory(), module.path(),
                                   "modules must be directories under system");
                const auto moduleName = module.path().filename().string();
                requireFeatureRule(isSnakeCase(moduleName), module.path(),
                                   "module directory must use snake_case");
                requireExpectedModule(entryName, moduleName, module.path());
                validateModuleFiles(module.path(), moduleName);
            }
        } else {
            requireExpectedModule(entryName, std::string_view{}, entry.path());
            validateModuleFiles(entry.path(), entryName);
        }
    }

    const auto systemPath = root / "system";
    requireFeatureRule(std::filesystem::is_directory(systemPath), systemPath,
                       "expected system module directory is missing");
    for (const auto module : rootModules)
        requireFeatureRule(std::filesystem::is_directory(root / module), root / module,
                           "expected root module is missing");
    for (const auto module : systemModules)
        requireFeatureRule(std::filesystem::is_directory(systemPath / module),
                           systemPath / module, "expected system module is missing");
}

void testFeatureFileLayout() {
    const auto root = repositoryRoot() / "service" / "features";
    const auto files = featureFiles();
    require(!files.empty(), "service/features contains no source files");
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        if (entry.is_directory())
            requireFeatureRule(isSnakeCase(entry.path().filename().string()), entry.path(),
                               "feature directory must use snake_case");
    for (const auto& path : files) {
        const auto relative = path.lexically_relative(root);
        std::vector<std::string> parts;
        for (const auto& part : relative)
            parts.emplace_back(part.string());
        requireFeatureRule(parts.size() <= 3, path,
                           "feature components may not be nested beyond one directory");
        requireFeatureRule(parts.size() == 2 || parts.size() == 3, path,
                           "feature files must be directly under a feature or component");

        const auto& feature = parts.front();
        const auto& owner = parts.size() == 2 ? feature : parts[1];
        const auto& file = parts.back();
        const auto prefix = owner + ".";
        requireFeatureRule(file.starts_with(prefix), path,
                           "feature file name must use its feature/component prefix");
        requireFeatureRule(whitelistedFeatureFile(file, owner), path,
                           "feature file name or extension is not whitelisted");
    }
}

void testCommonFileLayout() {
    const auto root = repositoryRoot() / "service" / "common";
    require(std::filesystem::is_directory(root), "service/common directory is missing");
    for (const auto& entry : std::filesystem::directory_iterator(root))
        requireFeatureRule(entry.is_regular_file(), entry.path(),
                           "service/common may contain foundation files only, no directories");
}

struct IncludeGraph {
    std::vector<std::filesystem::path> files;
    std::unordered_map<std::string, std::filesystem::path> paths;
    std::unordered_map<std::string, std::vector<std::string>> includes;
};

std::vector<std::filesystem::path> serviceSourceFiles() {
    const auto root = repositoryRoot() / "service";
    require(std::filesystem::is_directory(root), "service directory is missing");
    std::vector<std::filesystem::path> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
        if (entry.is_regular_file() && isArchitectureSource(entry.path()))
            result.push_back(entry.path());
    return result;
}

std::optional<std::string> includeTarget(std::string_view line) {
    auto cursor = line.find_first_not_of(" \t\r");
    if (cursor == std::string_view::npos || line[cursor] != '#')
        return std::nullopt;
    ++cursor;
    while (cursor < line.size() &&
           std::isspace(static_cast<unsigned char>(line[cursor])))
        ++cursor;
    constexpr std::string_view directive = "include";
    if (line.substr(cursor, directive.size()) != directive)
        return std::nullopt;
    cursor += directive.size();
    if (cursor < line.size() && !std::isspace(static_cast<unsigned char>(line[cursor])) &&
        line[cursor] != '"' && line[cursor] != '<')
        return std::nullopt;
    while (cursor < line.size() &&
           std::isspace(static_cast<unsigned char>(line[cursor])))
        ++cursor;
    if (cursor == line.size() || (line[cursor] != '"' && line[cursor] != '<'))
        return std::nullopt;
    const auto closing = line[cursor] == '"' ? '"' : '>';
    ++cursor;
    const auto end = line.find(closing, cursor);
    if (end == std::string_view::npos)
        return std::nullopt;
    return std::string(line.substr(cursor, end - cursor));
}

std::optional<std::string> resolveInclude(
    const std::filesystem::path& including,
    std::string_view spelling,
    const std::unordered_map<std::string, std::filesystem::path>& known) {
    std::string normalized(spelling);
    for (auto& character : normalized)
        if (character == '\\')
            character = '/';

    std::vector<std::filesystem::path> candidates;
    const auto target = std::filesystem::path(normalized);
    if (normalized.starts_with("service/"))
        candidates.push_back(repositoryRoot() / target);
    else {
        candidates.push_back(including.parent_path() / target);
        candidates.push_back(repositoryRoot() / "service" / target);
        candidates.push_back(repositoryRoot() / target);
    }

    for (const auto& candidate : candidates) {
        const auto lookup = [&](const std::filesystem::path& path)
            -> std::optional<std::string> {
            const auto key = pathKey(path.lexically_normal());
            if (known.contains(key))
                return key;
            return std::nullopt;
        };
        if (const auto result = lookup(candidate))
            return result;
        if (!candidate.has_extension()) {
            for (const auto suffix : {".h", ".hpp", ".hh", ".hxx", ".cpp", ".cc",
                                      ".cxx"}) {
                auto withSuffix = candidate;
                withSuffix += suffix;
                if (const auto result = lookup(withSuffix))
                    return result;
            }
        }
    }
    return std::nullopt;
}

bool isLayerReference(std::string_view spelling) {
    std::string normalized(spelling);
    for (auto& character : normalized)
        if (character == '\\')
            character = '/';
    return normalized.starts_with("service/modules/") ||
           normalized.starts_with("service/features/");
}

IncludeGraph buildIncludeGraph() {
    IncludeGraph graph;
    graph.files = serviceSourceFiles();
    require(!graph.files.empty(), "service contains no C++ source files");
    for (const auto& path : graph.files) {
        const auto key = pathKey(path);
        graph.paths.emplace(key, path);
        graph.includes.emplace(key, std::vector<std::string>{});
    }

    for (const auto& path : graph.files) {
        std::ifstream input(path, std::ios::binary);
        requireFeatureRule(input.good(), path, "cannot read service source for include graph");
        const auto from = pathKey(path);
        std::string line;
        while (std::getline(input, line)) {
            const auto target = includeTarget(line);
            if (!target)
                continue;
            const auto resolved = resolveInclude(path, *target, graph.paths);
            if (!resolved) {
                requireFeatureRule(!isLayerReference(*target), path,
                                   "service layer include target does not exist");
                continue;
            }
            auto& edges = graph.includes.at(from);
            if (std::find(edges.begin(), edges.end(), *resolved) == edges.end())
                edges.push_back(*resolved);
        }
    }
    return graph;
}

bool isModulePath(std::string_view key) {
    return key.starts_with("service/modules/");
}

bool isFeaturePath(std::string_view key) {
    return key.starts_with("service/features/");
}

bool isRuntimePath(std::string_view key) {
    const auto separator = key.find_last_of('/');
    const auto filename = key.substr(separator == std::string_view::npos ? 0 : separator + 1);
    return filename.ends_with(".runtime.h") || filename.ends_with(".runtime.cpp");
}

void requireNoLayerReachability(const IncludeGraph& graph, bool fromModules) {
    for (const auto& path : graph.files) {
        const auto start = pathKey(path);
        if (fromModules ? !isModulePath(start) : !isFeaturePath(start))
            continue;
        std::unordered_set<std::string> visited{start};
        std::vector<std::string> pending{start};
        while (!pending.empty()) {
            const auto current = std::move(pending.back());
            pending.pop_back();
            const auto edge = graph.includes.find(current);
            if (edge == graph.includes.end())
                continue;
            for (const auto& target : edge->second) {
                if (fromModules ? isFeaturePath(target) : isModulePath(target)) {
                    throw std::runtime_error(
                        std::string(fromModules ? "modules include graph reaches features: "
                                                : "features include graph reaches modules: ") +
                        start + " -> " + target);
                }
                if (visited.insert(target).second)
                    pending.push_back(target);
            }
        }
    }
}

void testIncludeGraphBoundaries() {
    const auto graph = buildIncludeGraph();
    requireNoLayerReachability(graph, true);
    requireNoLayerReachability(graph, false);

    for (const auto& path : graph.files) {
        const auto start = pathKey(path);
        if (!isFeaturePath(start) || !start.ends_with(".service.h"))
            continue;
        std::unordered_set<std::string> visited{start};
        std::vector<std::string> pending{start};
        while (!pending.empty()) {
            const auto current = std::move(pending.back());
            pending.pop_back();
            const auto edge = graph.includes.find(current);
            if (edge == graph.includes.end())
                continue;
            for (const auto& target : edge->second) {
                if (target != start && isRuntimePath(target))
                    throw std::runtime_error("feature service transitively includes runtime: " +
                                             start + " -> " + target);
                if (visited.insert(target).second)
                    pending.push_back(target);
            }
        }
    }

    for (const auto& path : graph.files) {
        const auto start = pathKey(path);
        if (!isFeaturePath(start))
            continue;
        std::unordered_map<std::string, unsigned char> state;
        std::function<void(const std::string&)> visit = [&](const std::string& current) {
            state[current] = 1;
            const auto edge = graph.includes.find(current);
            if (edge != graph.includes.end()) {
                for (const auto& target : edge->second) {
                    const auto targetState = state.contains(target) ? state.at(target) : 0;
                    if (targetState == 1)
                        throw std::runtime_error("feature include cycle: " + current +
                                                 " -> " + target);
                    if (targetState == 0)
                        visit(target);
                }
            }
            state[current] = 2;
        };
        visit(start);
    }
}

void testFeatureDomainBoundaries() {
    const auto files = featureFiles();
    static constexpr std::array<std::string_view, 6> forbiddenReferences{
        "service/domains/", "service/modules/", "../domains/",
        "../modules/",      "../../domains/",  "../../modules/",
    };
    for (const auto& path : files) {
        std::ifstream input(path, std::ios::binary);
        requireFeatureRule(input.good(), path, "cannot read feature source");
        const std::string content{std::istreambuf_iterator<char>(input),
                                  std::istreambuf_iterator<char>()};
        for (const auto reference : forbiddenReferences)
            requireFeatureRule(content.find(reference) == std::string::npos, path,
                               "feature directly references a domain/module path");
        if (path.filename().string().ends_with(".service.h"))
            requireFeatureRule(content.find(".runtime.h") == std::string::npos, path,
                               "feature service directly includes a runtime header");
    }
}

void testLifecycleOrder() {
    service::observability::RuntimeDiagnostics metrics;
    service::application::ComponentLifecycle runtime(metrics);
    std::vector<std::string> calls;
    runtime.add({.name = "database",
                 .start = [&] { calls.emplace_back("start-database"); },
                 .stop = [&] { calls.emplace_back("stop-database"); }});
    runtime.add({.name = "consumer",
                 .dependencies = {"database"},
                 .start = [&] { calls.emplace_back("start-consumer"); },
                 .stop = [&] { calls.emplace_back("stop-consumer"); }});
    runtime.start();
    require(metrics.areComponentsReady(), "runtime did not become ready");
    runtime.stop();
    const std::vector<std::string> expected{"start-database", "start-consumer",
                                             "stop-consumer", "stop-database"};
    require(calls == expected, "runtime lifecycle order is incorrect");
}

void testLifecycleRollback() {
    service::observability::RuntimeDiagnostics metrics;
    service::application::ComponentLifecycle runtime(metrics);
    bool stopped = false;
    runtime.add({.name = "first", .start = [] {}, .stop = [&] { stopped = true; }});
    runtime.add({.name = "broken",
                 .dependencies = {"first"},
                 .start = [] { throw std::runtime_error("expected"); },
                 .stop = [] {}});
    try {
        runtime.start();
        require(false, "runtime accepted a failed component");
    } catch (const std::runtime_error&) {
    }
    require(stopped, "runtime did not roll back started components");
    require(!metrics.areComponentsReady(), "failed runtime reported ready");
}

void testMessageEnvelope() {
    service::message::IngressPacket packet;
    packet.messageId = "event-1";
    packet.linkId = "link-1";
    packet.connectionId = "connection-1";
    const auto fields = service::message::ingressFields(packet);
    const auto field = [&](std::string_view name) {
        for (const auto& item : fields)
            if (item.name == name)
                return item.value;
        return std::string{};
    };
    require(field("event_id") == packet.messageId, "message envelope has no event id");
    require(field("schema_version") == "2", "message envelope has no schema version");
    require(field("event_type") == "packet", "message envelope has no event type");
}

void testExplicitOutbox() {
    const auto schema = source("service/config/schema.h");
    const auto dispatcher = source("service/features/messaging/messaging.service.h");
    require(schema.find("0023_transactional_outbox") != std::string::npos,
            "transactional outbox migration is missing");
    require(schema.find("0036_live_query_changes") != std::string::npos &&
                schema.find("publish_query_change") != std::string::npos,
            "query changes must be captured transactionally for external SQL writers");
    require(schema.find("0024_outbox_consumer_receipt") != std::string::npos,
            "outbox consumer receipt migration is missing");
    require(dispatcher.find("FOR UPDATE SKIP LOCKED") != std::string::npos,
            "outbox dispatcher is not safe for concurrent instances");
    for (const auto path : {"service/modules/device/device.service.h",
                            "service/modules/link/link.service.h",
                            "service/modules/protocol/protocol.service.h",
                            "service/modules/open_access/open_access.service.h"}) {
        const auto content = source(path);
        require(content.find("enqueueConfigEvent(transaction") != std::string::npos,
                "domain write does not enqueue an outbox event in its transaction");
        require(content.find("publishConfigEvent") == std::string::npos,
                "domain still directly publishes config events");
    }
}

void testOutboxOperations() {
    const auto reconciler = source("service/features/configuration/configuration.runtime.h");
    const auto webhook = source("service/features/access/access.runtime.h");
    for (const auto* consumer : {&reconciler, &webhook}) {
        require(consumer->find("idempotency::pending") != std::string::npos,
                "outbox consumer does not check durable receipts");
        require(consumer->find("idempotency::markProcessed") != std::string::npos,
                "outbox consumer does not persist durable receipts");
        require(consumer->find("idempotency::markProcessed") <
                    consumer->find("acknowledgeAndDeleteMany"),
                "outbox consumer acknowledges before persisting its receipt");
    }
    const auto controller = source("service/modules/system/outbox/outbox.controller.h");
    require(controller.find("/dead-letters/:id/replay") != std::string::npos,
            "dead-letter replay route is missing");
    const auto service = source("service/modules/system/outbox/outbox.service.h");
    require(service.find("system:outbox:manage") != std::string::npos,
            "dead-letter operations are not permission protected");
    require(service.find(".set(\"dead_lettered_at\", query.nullValue())") != std::string::npos,
            "dead-letter replay does not requeue the event");
    const auto dispatcher = source("service/features/messaging/messaging.service.h");
    require(dispatcher.find("DELETE FROM outbox_consumer_receipt") != std::string::npos,
            "outbox consumer receipts have no retention cleanup");
}

void testOperationalAlerts() {
    service::observability::RuntimeDiagnostics metrics;
    metrics.setComponentStatus("outbox", service::observability::ComponentState::Ready);
    require(metrics.setAlertState("outbox_dead_lettered", true, "value=1, threshold=1"),
            "activating an alert did not report a transition");
    require(metrics.healthJson().find("\"status\":\"degraded\"") != std::string::npos,
            "active operational alert does not degrade health status");
    require(metrics.prometheus().find(
                "iot_engine_alert_active{alert=\"outbox_dead_lettered\"} 1") !=
                std::string::npos,
            "active operational alert is missing from metrics");
    require(metrics.setAlertState("outbox_dead_lettered", false, "value=0, threshold=1"),
            "clearing an alert did not report a transition");
    require(metrics.healthJson().find("\"status\":\"ready\"") != std::string::npos,
            "cleared operational alert did not restore health status");
}

void testInjectedSharedState() {
    const auto auth = source("service/modules/system/auth/auth.service.h");
    require(auth.find("explicit AuthService(LoginRateLimiter& limiter)") != std::string::npos,
            "auth service does not inject its rate limiter");
    require(auth.find("iot:auth:login-failures:") != std::string::npos,
            "login rate limiting is not shared through Redis");
    require(auth.find("unordered_map") == std::string::npos,
            "login rate limiting still uses process-local state");
    const auto command = source("service/modules/command/command.service.h");
    require(command.find("explicit CommandService(service::device::DeviceAccessService&") !=
                std::string::npos,
            "command service does not inject device authorization");
}

void testSymmetricServiceWorkers() {
    const auto server = source("service/server.cpp");
    require(server.find("workers.front()") == std::string::npos &&
                server.find("workers.back()") == std::string::npos,
            "a Service task is assigned to a privileged Worker");
    require(server.find("registerServiceWorkerLifecycle(*owner, worker, index, count, collectors)") != std::string::npos,
            "Service Workers do not use the same component assembly");
    require(server.find("std::vector<std::shared_ptr<ServiceWorkerComponents>> workers") != std::string::npos,
            "Service Workers share one component owner");

    const auto freshness = source("service/features/telemetry/telemetry.runtime.h");
    const auto reconciler = source("service/features/configuration/configuration.runtime.h");
    const auto webhook = source("service/features/access/access.runtime.h");
    const auto edge = source("service/features/edge/edge.runtime.h");
    for (const auto* component : {&freshness, &reconciler, &webhook, &edge}) {
        require(component->find("workers_.front().post") == std::string::npos &&
                    component->find("workers_.back().post") == std::string::npos,
                "a worker-local task still selects front/back");
        require(component->find("+= serviceWorkerCount_") == std::string::npos,
                "business work is still assigned by fixed worker partitions");
        require(component->find("claimGroupMany") != std::string::npos,
                "a background task cannot recover previously claimed work");
    }
    require(freshness.find("WorkerStreamTask::Freshness") != std::string::npos,
            "freshness task does not wait on its Worker-local wake channel");
    require(reconciler.find("runtimeConfigChangesStream()") !=
                std::string::npos,
            "runtime reconciliation does not use a shared claimed-work queue");
    require(webhook.find("stream::event()") != std::string::npos,
             "webhook delivery does not use independent work claims");
    require(webhook.find("stream::sessionChanges()") !=
                std::string::npos &&
                webhook.find("session::ensure(context)") != std::string::npos,
            "access-session projection does not claim shared work with idempotent startup");
    require(edge.find("projector_stream::stream(index)") != std::string::npos,
             "edge projection is not isolated by accepting Worker");

    for (const auto path : {
             "service/utils/redis.h",
             "service/common/message.h",
             "service/features/access/access.runtime.h",
             "service/features/command/command.runtime.h",
             "service/features/configuration/configuration.runtime.h",
             "service/features/telemetry/telemetry.runtime.h",
             "service/features/edge/edge.runtime.h",
         }) {
        const auto content = source(path);
        require(content.find("repairLegacyReadSet") == std::string::npos &&
                    content.find("eraseDrainedStream") == std::string::npos &&
                    content.find("message::shard") == std::string::npos &&
                    content.find("legacyStream") == std::string::npos,
                "worker queues still contain removed compatibility paths");
    }
}

void testWorkerStreamMultiplexing() {
    const auto server = source("service/server.cpp");
    const auto multiplexer =
        source("service/features/messaging/stream_multiplexer/stream_multiplexer.runtime.h");
    require(server.find("serviceRedis.blockingPoolSizePerWorker = 4") !=
                std::string::npos,
            "Service Workers need bounded multiplexer, feature live, API live and RPC readers");
    require(server.find("owner->multiplexer->configure(worker, index)") != std::string::npos &&
                server.find("m->start(worker, index)") != std::string::npos,
            "worker-local Stream multiplexer is not in the application lifecycle");
    require(multiplexer.find("thread_local WorkerStreamMultiplexer*") != std::string::npos &&
                multiplexer.find("std::vector<ruvia::WebWorkerHandle>") == std::string::npos,
            "Stream wake channels are shared across Service Workers");
    require(service::rpc::Contract::requests("instance", 0) !=
                service::rpc::Contract::requests("instance", 1),
            "RPC requests can be consumed by another Service Worker");
    require(multiplexer.find("readGroupManyBlockingUntil") != std::string::npos &&
                multiplexer.find("workerWakeStream(index)") != std::string::npos &&
                multiplexer.find(".capacity = 1") != std::string::npos,
            "Stream multiplexer does not use one coalescing blocker per Worker");
    const auto stream = source("service/features/messaging/messaging.transport.h");
    require(stream.find("kAddAndWakeScript") != std::string::npos &&
                stream.find("atomic XADD/wake") != std::string::npos,
            "business messages and Worker wakeups are not published atomically");

    for (const auto path : {
             "service/features/telemetry/telemetry.runtime.h",
             "service/features/command/command.runtime.h",
             "service/features/access/access.runtime.h",
             "service/features/configuration/configuration.runtime.h",
             "service/features/edge/edge.runtime.h",
             "service/features/edge/edge.runtime.h",
        }) {
        const auto consumer = source(path);
        require(consumer.find("workerStreamMultiplexer().wait") !=
                        std::string::npos,
                "Service Stream consumer does not use the worker-local multiplexer");
        require(consumer.find("readGroupManyBlocking") == std::string::npos &&
                    consumer.find("readGroupBlocking") == std::string::npos,
                "Service Stream consumer still owns a Redis blocking connection");
    }
}

} // namespace

int main() {
    try {
        testServiceTopLevelLayout();
        testModuleFileLayout();
        testFeatureFileLayout();
        testCommonFileLayout();
        testIncludeGraphBoundaries();
        testFeatureDomainBoundaries();
        testLifecycleOrder();
        testLifecycleRollback();
        testMessageEnvelope();
        testExplicitOutbox();
        testOutboxOperations();
        testOperationalAlerts();
        testInjectedSharedState();
        testSymmetricServiceWorkers();
        testWorkerStreamMultiplexing();
        std::cout << "architecture tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "architecture test failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
