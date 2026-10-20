#pragma once
#include "fl/core/Config.h"

namespace fl {
class NvsConfigRepository final : public IConfigRepository {
public:
    Result load(Config&) override;
    Result save(const Config&) override;
    Result loadRegistry(Registry&) override;
    Result saveRegistry(const Registry&) override;
    Result loadGroups(List<GroupConfig, kMaxGroups>&) override;
    Result saveGroups(const List<GroupConfig, kMaxGroups>&) override;
};
} // namespace fl
