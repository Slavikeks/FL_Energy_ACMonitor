#pragma once

#include "fl/core/Config.h"

namespace fl {
inline constexpr std::size_t kGroupStorageMaxBytes = 4000;

Result validateGroupSet(const Config&, const List<GroupConfig, kMaxGroups>&);
Result encodeGroups(const List<GroupConfig, kMaxGroups>&, std::uint8_t*, std::size_t,
                    std::size_t& written);
Result decodeGroups(const std::uint8_t*, std::size_t, List<GroupConfig, kMaxGroups>&);
} // namespace fl
