#include "fl/rotation/GroupCodec.h"
#include <cstring>
#include <string_view>

namespace fl {
namespace {
std::uint32_t crc32(const std::uint8_t* data, std::size_t length) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}
class Writer {
public:
    Writer(std::uint8_t* out, std::size_t capacity) : out_(out), capacity_(capacity) {}
    bool byte(std::uint8_t value) { if (size_ >= capacity_) return false; out_[size_++] = value; return true; }
    bool word(std::uint16_t value) { return byte(value & 0xff) && byte(value >> 8); }
    bool dword(std::uint32_t value) { return word(value & 0xffff) && word(value >> 16); }
    bool qword(std::uint64_t value) { return dword(value & 0xffffffffU) && dword(value >> 32); }
    bool text(std::string_view value) {
        if (value.size() > 96 || !byte(static_cast<std::uint8_t>(value.size())) || size_ + value.size() > capacity_) return false;
        for (char c : value) out_[size_++] = static_cast<std::uint8_t>(c);
        return true;
    }
    std::size_t size() const { return size_; }
private:
    std::uint8_t* out_; std::size_t capacity_; std::size_t size_{0};
};
class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t length) : data_(data), length_(length) {}
    bool byte(std::uint8_t& value) { if (offset_ >= length_) return false; value = data_[offset_++]; return true; }
    bool word(std::uint16_t& value) { std::uint8_t a, b; if (!byte(a) || !byte(b)) return false; value = a | (std::uint16_t(b) << 8); return true; }
    bool dword(std::uint32_t& value) { std::uint16_t a, b; if (!word(a) || !word(b)) return false; value = a | (std::uint32_t(b) << 16); return true; }
    bool qword(std::uint64_t& value) { std::uint32_t a, b; if (!dword(a) || !dword(b)) return false; value = a | (std::uint64_t(b) << 32); return true; }
    bool text(Name& value) {
        std::uint8_t size; if (!byte(size) || size == 0 || size > 96 || offset_ + size > length_) return false;
        const bool ok = value.assign(std::string_view(reinterpret_cast<const char*>(data_ + offset_), size));
        offset_ += size; return ok;
    }
    bool done() const { return offset_ == length_; }
private:
    const std::uint8_t* data_; std::size_t length_; std::size_t offset_{0};
};
}

Result validateGroupSet(const Config& config, const List<GroupConfig, kMaxGroups>& groups) {
    if (groups.size() > kMaxGroups) return Result::Full;
    for (std::size_t i = 0; i < groups.size(); ++i) {
        const auto& group = groups[i];
        if (!group.id || group.name.view().empty() || group.members.size() == 0 || group.members.size() > kMaxAc ||
            group.required_active == 0 || group.required_active > group.maximum_active ||
            group.maximum_active > group.members.size() || static_cast<unsigned>(group.mode) > 1 ||
            static_cast<unsigned>(group.ac_mode) > 3 || static_cast<unsigned>(group.fan) > 3 ||
            group.color_rgb > 0xFFFFFFU ||
            group.setpoint_deci_c < 160 || group.setpoint_deci_c > 300 || group.setpoint_deci_c % 10 != 0 || group.interval_days == 0 ||
            group.rotation_hour > 23 || group.rotation_minute > 59 ||
            group.high_temp_deci_c <= group.recovery_temp_deci_c ||
            group.start_verification_s == 0 || group.min_on_s == 0 || group.min_off_s == 0)
            return Result::Invalid;
        if (group.rotation_enabled && (group.mode != GroupMode::Auto ||
            (group.handover_s > 0 && group.maximum_active < group.required_active + 1))) return Result::Invalid;
        if (group.ventilation_reserve && (group.mode != GroupMode::Auto || group.ac_mode == Mode::Fan))
            return Result::Invalid;
        for (std::size_t j = 0; j < i; ++j) if (groups[j].id == group.id) return Result::Invalid;
        for (std::size_t member_index = 0; member_index < group.members.size(); ++member_index) {
            const auto address = group.members[member_index];
            if (!validAddress(address)) return Result::Invalid;
            bool registered = false;
            for (const auto& ac : config.registry) if (ac.address == address) { registered = true; break; }
            if (!registered) return Result::NotFound;
            for (std::size_t k = 0; k < member_index; ++k)
                if (group.members[k] == address) return Result::Invalid;
            for (std::size_t prior_group = 0; prior_group < i; ++prior_group)
                for (const auto prior_address : groups[prior_group].members)
                    if (prior_address == address) return Result::Invalid;
        }
    }
    return Result::Accepted;
}

Result encodeGroups(const List<GroupConfig, kMaxGroups>& groups, std::uint8_t* out,
                    std::size_t capacity, std::size_t& written) {
    written = 0;
    if (!out || capacity < 10 || groups.size() > kMaxGroups) return Result::Invalid;
    Writer writer(out, capacity - 4);
    for (char c : std::string_view("FLG1")) if (!writer.byte(static_cast<std::uint8_t>(c))) return Result::Full;
    if (!writer.byte(2) || !writer.byte(static_cast<std::uint8_t>(groups.size()))) return Result::Full;
    for (const auto& g : groups) {
        const std::uint8_t flags = (g.rotation_enabled ? 1 : 0) | (g.last_rotation_anchor ? 2 : 0) |
            (g.ventilation_reserve ? 4 : 0);
        if (!writer.word(g.id) || !writer.byte(static_cast<std::uint8_t>(g.mode)) ||
            !writer.byte(g.required_active) || !writer.byte(g.maximum_active) ||
            !writer.byte(static_cast<std::uint8_t>(g.ac_mode)) || !writer.byte(static_cast<std::uint8_t>(g.fan)) ||
            !writer.byte(flags) || !writer.word(g.interval_days) || !writer.byte(g.rotation_hour) ||
            !writer.byte(g.rotation_minute) || !writer.word(static_cast<std::uint16_t>(g.setpoint_deci_c)) ||
            !writer.word(static_cast<std::uint16_t>(g.high_temp_deci_c)) || !writer.dword(g.high_delay_s) ||
            !writer.word(static_cast<std::uint16_t>(g.recovery_temp_deci_c)) || !writer.dword(g.recovery_delay_s) ||
            !writer.dword(g.handover_s) || !writer.dword(g.start_verification_s) ||
            !writer.dword(g.min_on_s) || !writer.dword(g.min_off_s) ||
            !writer.byte(static_cast<std::uint8_t>(g.color_rgb >> 16)) ||
            !writer.byte(static_cast<std::uint8_t>(g.color_rgb >> 8)) ||
            !writer.byte(static_cast<std::uint8_t>(g.color_rgb)) ||
            (g.last_rotation_anchor && !writer.qword(static_cast<std::uint64_t>(*g.last_rotation_anchor))) ||
            !writer.text(g.name.view()) || !writer.byte(static_cast<std::uint8_t>(g.members.size()))) return Result::Full;
        for (const auto member : g.members) if (!writer.byte(member)) return Result::Full;
    }
    const auto crc = crc32(out, writer.size());
    if (writer.size() + 4 > capacity) return Result::Full;
    for (unsigned i = 0; i < 4; ++i) out[writer.size() + i] = static_cast<std::uint8_t>(crc >> (8 * i));
    written = writer.size() + 4;
    return Result::Accepted;
}

Result decodeGroups(const std::uint8_t* bytes, std::size_t length,
                    List<GroupConfig, kMaxGroups>& output) {
    if (!bytes || length < 10 || length > kGroupStorageMaxBytes || std::memcmp(bytes, "FLG1", 4) ||
        (bytes[4] != 1 && bytes[4] != 2) || bytes[5] > kMaxGroups)
        return Result::Invalid;
    const std::uint8_t format_version = bytes[4];
    const std::uint32_t expected = std::uint32_t(bytes[length - 4]) | (std::uint32_t(bytes[length - 3]) << 8) |
        (std::uint32_t(bytes[length - 2]) << 16) | (std::uint32_t(bytes[length - 1]) << 24);
    if (crc32(bytes, length - 4) != expected) return Result::Invalid;
    Reader reader(bytes + 6, length - 10);
    List<GroupConfig, kMaxGroups> decoded;
    for (std::size_t i = 0; i < bytes[5]; ++i) {
        GroupConfig g; std::uint8_t mode, required, maximum, ac_mode, fan, flags, hour, minute, member_count;
        std::uint16_t value16; std::uint64_t anchor;
        if (!reader.word(g.id) || !reader.byte(mode) || !reader.byte(required) || !reader.byte(maximum) ||
            !reader.byte(ac_mode) || !reader.byte(fan) || !reader.byte(flags) || !reader.word(g.interval_days) ||
            !reader.byte(hour) || !reader.byte(minute) || !reader.word(value16)) return Result::Invalid;
        g.mode = static_cast<GroupMode>(mode); g.required_active = required; g.maximum_active = maximum;
        g.ac_mode = static_cast<Mode>(ac_mode); g.fan = static_cast<Fan>(fan); g.rotation_enabled = flags & 1;
        g.ventilation_reserve = format_version >= 2 && (flags & 4);
        g.rotation_hour = hour; g.rotation_minute = minute; g.setpoint_deci_c = static_cast<std::int16_t>(value16);
        if (!reader.word(value16)) return Result::Invalid;
        g.high_temp_deci_c = static_cast<std::int16_t>(value16);
        if (!reader.dword(g.high_delay_s) || !reader.word(value16)) return Result::Invalid;
        g.recovery_temp_deci_c = static_cast<std::int16_t>(value16);
        if (!reader.dword(g.recovery_delay_s) || !reader.dword(g.handover_s) ||
            !reader.dword(g.start_verification_s) || !reader.dword(g.min_on_s) ||
            !reader.dword(g.min_off_s)) return Result::Invalid;
        if (format_version >= 2) {
            std::uint8_t red, green, blue;
            if (!reader.byte(red) || !reader.byte(green) || !reader.byte(blue)) return Result::Invalid;
            g.color_rgb = (static_cast<std::uint32_t>(red) << 16) |
                (static_cast<std::uint32_t>(green) << 8) | blue;
        }
        if (flags & 2) { if (!reader.qword(anchor)) return Result::Invalid; g.last_rotation_anchor = static_cast<EpochSeconds>(anchor); }
        if (!reader.text(g.name) || !reader.byte(member_count) || member_count > kMaxAc || member_count == 0) return Result::Invalid;
        for (std::size_t m = 0; m < member_count; ++m) { std::uint8_t address; if (!reader.byte(address) || !g.members.push_back(address)) return Result::Invalid; }
        if (!decoded.push_back(g)) return Result::Full;
    }
    if (!reader.done()) return Result::Invalid;
    output = decoded;
    return Result::Accepted;
}
} // namespace fl
