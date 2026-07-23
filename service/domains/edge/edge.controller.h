#pragma once

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include <openssl/evp.h>
#include <ruvia/web/Controller.h>

#include "service/common/http.h"
#include "service/common/uuid.h"
#include "service/middleware/auth.h"
#include "service/middleware/permission.h"
#include "service/features/edge/config.h"
#include "service/domains/edge/edge.schema.h"
#include "service/domains/edge/edge.service.h"

namespace service::edge {

class EdgeController final : public ruvia::Controller<EdgeController> {
  public:
    RUVIA_CONTROLLER_GROUP("/api/edge", service::middleware::AuthMiddleware)
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/", list, EdgeListValidator);
    RUVIA_GET("/firmware", firmwares);
    RUVIA_PUT("/:id/enrollment", enrollment, EdgeIdValidator, EnrollmentValidator);
    RUVIA_PUT("/:id/name", renameNode, EdgeIdValidator, NodeNameValidator);
    RUVIA_POST("/:id/network", network, EdgeIdValidator, NetworkValidator);
    RUVIA_POST("/:id/modem", modem, EdgeIdValidator, ModemControlValidator);
    RUVIA_POST("/:id/sync", sync, EdgeIdValidator);
    RUVIA_POST("/:id/platforms", savePlatform, EdgeIdValidator, PlatformValidator);
    RUVIA_DELETE("/:id/platforms/:platformId", removePlatform,
                 EdgePlatformParamsValidator);
    RUVIA_POST_STREAM("/:id/firmware", uploadFirmware, EdgeIdValidator);
    RUVIA_POST("/:id/terminal-ticket", terminalTicket, EdgeIdValidator);
    RUVIA_GET("/:id", detail, EdgeIdValidator);
    RUVIA_ROUTES_END

  private:
    static std::string id(ruvia::Context& c) {
        return std::string(c.req().valid<EdgeIdParams>().id()->view());
    }

    static std::optional<std::string> text(const std::optional<ruvia::String>& value) {
        return value ? std::optional<std::string>(std::string(value->view())) : std::nullopt;
    }

    ruvia::Task<ruvia::HttpResponse> list(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:edge:query");
        const auto& query = c.req().valid<EdgeListQuery>();
        co_return c.json(service::common::ok<EdgePageResponse>(
            c, co_await edgeService().list(c, *query.page(), *query.pageSize(),
                                           text(query.keyword()), text(query.status()))));
    }

    ruvia::Task<ruvia::HttpResponse> detail(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:edge:query");
        co_return c.json(service::common::ok<EdgeDetailResponse>(
            c, co_await edgeService().detail(c, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> enrollment(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:edge:edit");
        co_await edgeService().setEnrollment(c, id(c), c.req().valid<EnrollmentBody>());
        co_return c.json(service::common::operation(c, "注册状态已更新"));
    }

    ruvia::Task<ruvia::HttpResponse> renameNode(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:edge:edit");
        co_await edgeService().renameNode(c, id(c), c.req().valid<NodeNameBody>());
        co_return c.json(service::common::operation(c, "节点名称已更新"));
    }

    ruvia::Task<ruvia::HttpResponse> network(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:edge:config");
        co_await edgeService().queueNetwork(c, id(c), c.req().valid<NetworkBody>());
        co_return c.json(service::common::operation(c, "网络配置已下发"));
    }

    ruvia::Task<ruvia::HttpResponse> modem(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:edge:config");
        co_await edgeService().queueModem(c, id(c), c.req().valid<ModemControlBody>());
        co_return c.json(service::common::operation(c, "4G 操作已下发"));
    }

    ruvia::Task<ruvia::HttpResponse> sync(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:edge:config");
        (void)co_await configService().queueSnapshot(c, id(c));
        co_return c.json(service::common::operation(c, "设备配置已生成并下发"));
    }

    ruvia::Task<ruvia::HttpResponse> savePlatform(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:edge:config");
        (void)co_await edgeService().queuePlatform(c, id(c), c.req().valid<PlatformBody>());
        co_return c.json(service::common::operation(c, "平台配置已下发"));
    }

    ruvia::Task<ruvia::HttpResponse> removePlatform(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:edge:config");
        const auto& params = c.req().valid<EdgePlatformParams>();
        co_await edgeService().deletePlatform(c, params.id()->view(), params.platformId()->view());
        co_return c.json(service::common::operation(c, "平台删除配置已下发"));
    }

    ruvia::Task<ruvia::HttpResponse> firmwares(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:edge:query");
        co_return c.json(service::common::ok<FirmwareListResponse>(
            c, co_await edgeService().firmwares(c)));
    }

    ruvia::Task<ruvia::HttpResponse> terminalTicket(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:edge:terminal");
        co_return c.json(service::common::ok<TerminalTicketResponse>(
            c, co_await edgeService().terminalTicket(c, id(c))));
    }

    ruvia::Task<ruvia::HttpResponse> uploadFirmware(ruvia::Context& c) {
        co_await service::middleware::requirePermission(c, "iot:edge:firmware");
        const auto nodeId = id(c);
        co_await edgeService().validateFirmwareTarget(c, nodeId);
        const auto configured = c.env().get("EDGE_FIRMWARE_DIR").value_or("firmware");
        std::error_code error;
        auto directory = std::filesystem::absolute(std::filesystem::path(configured), error);
        if (error || !std::filesystem::create_directories(directory, error) && error)
            service::common::fail(17011, "无法创建固件目录", 500);
        const auto storageId = service::common::nextUuidV7();
        const auto outputPath = directory / (storageId + ".bin");
        struct Cleanup {
            std::filesystem::path path;
            bool keep{};
            ~Cleanup() {
                if (!keep) {
                    std::error_code ignored;
                    std::filesystem::remove(path, ignored);
                }
            }
        } cleanup{outputPath};

        std::ofstream output;
        std::string keepSettingsText;
        std::string fileName;
        std::uint64_t bytes = 0;
        bool fileSeen = false;
        using DigestPtr = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
        DigestPtr digest(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
        if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
            service::common::fail(17012, "无法初始化固件摘要", 500);

        auto reader = c.req().multipartReader();
        while (auto part = co_await reader.read()) {
            const auto phase = part->phase();
            const bool begins = phase == ruvia::MultipartChunkPhase::kFirst ||
                                phase == ruvia::MultipartChunkPhase::kComplete;
            const bool ends = phase == ruvia::MultipartChunkPhase::kLast ||
                              phase == ruvia::MultipartChunkPhase::kComplete;
            if (begins && part->name() == "file") {
                if (fileSeen)
                    service::common::fail(17013, "只能上传一个固件文件", 400);
                fileSeen = true;
                fileName = std::filesystem::path(std::string(part->filename())).filename().string();
                output.open(outputPath, std::ios::binary | std::ios::trunc);
                if (!output)
                    service::common::fail(17014, "无法保存固件文件", 500);
            }
            if (part->name() == "keepSettings") {
                keepSettingsText.append(part->body());
                if (keepSettingsText.size() > 5)
                    service::common::fail(17018, "保留配置参数无效", 400);
            } else if (part->name() == "file" && fileSeen) {
                bytes += part->body().size();
                if (bytes > 128ULL * 1024ULL * 1024ULL)
                    service::common::fail(17016, "固件不能超过 128 MiB", 400);
                output.write(part->body().data(), static_cast<std::streamsize>(part->body().size()));
                if (!output || EVP_DigestUpdate(digest.get(), part->body().data(),
                                                part->body().size()) != 1)
                    service::common::fail(17014, "固件保存或摘要计算失败", 500);
                if (ends)
                    output.close();
            }
        }
        if (!fileSeen || bytes == 0 || fileName.empty())
            service::common::fail(17017, "固件文件不能为空", 400);
        auto version = std::filesystem::path(fileName).stem().string();
        if (version.empty())
            service::common::fail(17017, "无法从固件文件名识别版本", 400);
        if (version.size() > 64)
            service::common::fail(17015, "固件文件名生成的版本不能超过 64 个字符", 400);
        const bool keepSettings = keepSettingsText.empty() || keepSettingsText == "true" ||
                                  keepSettingsText == "1";
        if (!keepSettings && keepSettingsText != "false" && keepSettingsText != "0")
            service::common::fail(17018, "保留配置参数无效", 400);
        std::array<unsigned char, 32> hash{};
        unsigned hashSize = 0;
        if (EVP_DigestFinal_ex(digest.get(), hash.data(), &hashSize) != 1 ||
            hashSize != hash.size())
            service::common::fail(17012, "固件摘要计算失败", 500);
        constexpr char digits[] = "0123456789abcdef";
        std::string hashText;
        hashText.reserve(64);
        for (const auto value : hash) {
            hashText.push_back(digits[value >> 4U]);
            hashText.push_back(digits[value & 0x0fU]);
        }
        co_await edgeService().registerFirmware(
            c, storageId, std::move(version), std::move(fileName), outputPath,
            std::move(hashText), static_cast<std::int64_t>(bytes));
        cleanup.keep = true;
        co_await edgeService().queueFirmware(c, nodeId, storageId, keepSettings);
        co_return c.json(service::common::operation(c, "固件已上传，刷写任务已下发给当前节点"));
    }
};

class EdgePublicController final : public ruvia::Controller<EdgePublicController> {
  public:
    RUVIA_CONTROLLER_GROUP("/edge/v1/firmware")
    RUVIA_ROUTES_BEGIN
    RUVIA_GET("/:id/download", download, EdgeIdValidator, FirmwareDownloadValidator);
    RUVIA_ROUTES_END

  private:
    ruvia::Task<ruvia::HttpResponse> download(ruvia::Context& c) {
        const auto& id = c.req().valid<EdgeIdParams>();
        const auto& query = c.req().valid<FirmwareDownloadQuery>();
        auto [path, fileName] = co_await edgeService().firmwareDownload(
            c, id.id()->view(), query.token()->view());
        if (!std::filesystem::is_regular_file(path))
            service::common::fail(17009, "固件文件不存在", 404);
        c.header("Content-Disposition", "attachment; filename=firmware.bin");
        co_return c.file(path, "application/octet-stream");
    }
};

} // namespace service::edge
