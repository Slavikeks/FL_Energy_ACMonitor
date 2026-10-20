#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fl {
namespace detail {
inline const char* transliterateCyrillic(std::uint32_t cp) {
    if (cp == 0x0401) return "Yo";
    if (cp == 0x0451) return "yo";
    if (cp == 0x042A || cp == 0x042C) return "";
    if (cp == 0x044A || cp == 0x044C) return "";
    static constexpr const char* upper[] = {
        "A", "B", "V", "G", "D", "E", "Zh", "Z", "I", "Y",
        "K", "L", "M", "N", "O", "P", "R", "S", "T", "U",
        "F", "Kh", "Ts", "Ch", "Sh", "Shch", "", "Y", "", "E", "Yu", "Ya"
    };
    static constexpr const char* lower[] = {
        "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y",
        "k", "l", "m", "n", "o", "p", "r", "s", "t", "u",
        "f", "kh", "ts", "ch", "sh", "shch", "", "y", "", "e", "yu", "ya"
    };
    if (cp >= 0x0410 && cp <= 0x042F) return upper[cp - 0x0410];
    if (cp >= 0x0430 && cp <= 0x044F) return lower[cp - 0x0430];
    return nullptr;
}
} // namespace detail

// SNMP DisplayString is safest across legacy managers when kept to printable
// ASCII. Russian UTF-8 names are transliterated; unknown characters become '?'.
inline std::size_t toSnmpDisplayText(std::string_view input, char* output, std::size_t capacity) {
    if (!output || capacity == 0) return 0;
    std::size_t in = 0;
    std::size_t out = 0;
    auto append = [&](char c) {
        if (out + 1 < capacity) output[out++] = c;
    };
    while (in < input.size() && out + 1 < capacity) {
        const auto first = static_cast<unsigned char>(input[in]);
        if (first < 0x80) {
            append(first >= 0x20 && first <= 0x7E ? static_cast<char>(first) : ' ');
            ++in;
            continue;
        }
        unsigned length = 0;
        std::uint32_t cp = 0;
        if (first >= 0xC2 && first <= 0xDF) { length = 2; cp = first & 0x1F; }
        else if (first >= 0xE0 && first <= 0xEF) { length = 3; cp = first & 0x0F; }
        else if (first >= 0xF0 && first <= 0xF4) { length = 4; cp = first & 0x07; }
        else { append('?'); ++in; continue; }
        if (in + length > input.size()) { append('?'); ++in; continue; }
        bool valid = true;
        for (unsigned i = 1; i < length; ++i) {
            const auto next = static_cast<unsigned char>(input[in + i]);
            if ((next & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (next & 0x3F);
        }
        if (!valid || (length == 3 && cp < 0x800) || (length == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            append('?'); ++in; continue;
        }
        const char* transliterated = detail::transliterateCyrillic(cp);
        if (!transliterated) append('?');
        else while (*transliterated && out + 1 < capacity) append(*transliterated++);
        in += length;
    }
    output[out] = '\0';
    return out;
}
} // namespace fl
