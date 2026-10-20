#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace fl {

// UTF-8 bytes, not code points. Reject oversized input instead of truncating it.
template <std::size_t MaxBytes> class Text {
public:
    Text() = default;
    explicit Text(std::string_view value) { assign(value); }
    bool assign(std::string_view value) {
        if (value.size() > MaxBytes || value.find('\0') != std::string_view::npos) {
            return false;
        }
        for (std::size_t i = 0; i < value.size(); ++i) bytes_[i] = value[i];
        length_ = value.size();
        bytes_[length_] = '\0';
        return true;
    }
    std::string_view view() const { return {bytes_.data(), length_}; }
    const char* c_str() const { return bytes_.data(); }
private:
    std::array<char, MaxBytes + 1> bytes_{};
    std::size_t length_{0};
};

// Logical size is dynamic, physical allocation is bounded and heap-free.
template <typename T, std::size_t Capacity> class List {
public:
    bool push_back(const T& item) {
        if (size_ == Capacity) return false;
        items_[size_++] = item;
        return true;
    }
    bool erase(std::size_t index) {
        if (index >= size_) return false;
        for (std::size_t i = index + 1; i < size_; ++i) items_[i - 1] = items_[i];
        items_[--size_] = T{};
        return true;
    }
    void clear() { while (size_ != 0) items_[--size_] = T{}; }
    std::size_t size() const { return size_; }
    static constexpr std::size_t capacity() { return Capacity; }
    T& operator[](std::size_t index) { return items_[index]; }
    const T& operator[](std::size_t index) const { return items_[index]; }
    T* begin() { return items_.data(); }
    T* end() { return items_.data() + size_; }
    const T* begin() const { return items_.data(); }
    const T* end() const { return items_.data() + size_; }
private:
    std::array<T, Capacity> items_{};
    std::size_t size_{0};
};
} // namespace fl
