#pragma once
#include <ruvia/web/Controller.h>
#include "service/common/http.h"
namespace service::application {
enum class Role { Api, Media, Vpn };
inline Role parseRole(std::string_view value) {
    if(value=="api")return Role::Api;
    if(value=="media")return Role::Media;
    if(value=="vpn")return Role::Vpn;
    throw std::invalid_argument("SERVICE_ROLE must be api, media, or vpn");
}
class RoleBoundary final : public ruvia::Middleware<RoleBoundary> {
public:
    explicit RoleBoundary(Role role):role_(role){}
    ruvia::Task<void> handle(ruvia::Context& c,ruvia::Next& next) {
        const auto path=c.req().path();
        const auto under=[&](std::string_view prefix){return path==prefix || (path.starts_with(prefix) && path.size()>prefix.size() && path[prefix.size()]=='/');};
        const bool media=under("/v1/gb28181") || under("/media");
        const bool vpn=under("/v1/vpn");
        const bool probe=under("/internal/health") || path=="/internal/metrics";
        if(!probe && ((role_==Role::Api && (media||vpn)) ||
            (role_==Role::Media && !media) || (role_==Role::Vpn && !vpn)))
            service::common::fail(10001,"Route is not served by this process",404);
        co_await next();
    }
private:Role role_;
};
}
