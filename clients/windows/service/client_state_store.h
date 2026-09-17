#pragma once
#include "../common/json.h"
namespace iotvpn::service {
struct IClientStateStore {
    virtual ~IClientStateStore() = default;
    virtual Json load() = 0;
    virtual void save(const Json&) = 0;
};
class DpapiClientStateStore final : public IClientStateStore {
public:
    Json load() override;
    void save(const Json& state) override;
};

}
