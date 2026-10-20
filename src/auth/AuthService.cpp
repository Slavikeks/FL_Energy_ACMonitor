#include "fl/auth/AuthService.h"
#include <mbedtls/md.h>
#include <mbedtls/pkcs5.h>
#include <esp_system.h>
#include <cstdio>
#include <cstring>

namespace fl {
namespace {
constexpr std::uint32_t kPbkdf2Iterations = 12000;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kDigestBytes = 32;
int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool decodeHex(std::string_view input, std::uint8_t* output, std::size_t count) {
    if (input.size() != count * 2) return false;
    for (std::size_t i = 0; i < count; ++i) {
        const int hi = hexNibble(input[i * 2]), lo = hexNibble(input[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        output[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return true;
}
void encodeHex(const std::uint8_t* input, std::size_t size, char* output) {
    static constexpr char alphabet[] = "0123456789abcdef";
    for (std::size_t i = 0; i < size; ++i) {
        output[2 * i] = alphabet[input[i] >> 4];
        output[2 * i + 1] = alphabet[input[i] & 0x0f];
    }
}
bool constantTimeEqual(const std::uint8_t* left, const std::uint8_t* right, std::size_t size) {
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < size; ++i) difference |= left[i] ^ right[i];
    return difference == 0;
}
bool derive(std::string_view password, const std::uint8_t* salt, std::uint8_t* digest) {
    const auto* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return false;
    mbedtls_md_context_t context;
    mbedtls_md_init(&context);
    if (mbedtls_md_setup(&context, info, 1) != 0) { mbedtls_md_free(&context); return false; }
    const int result = mbedtls_pkcs5_pbkdf2_hmac(&context,
        reinterpret_cast<const unsigned char*>(password.data()), password.size(),
        salt, kSaltBytes, kPbkdf2Iterations, kDigestBytes, digest);
    mbedtls_md_free(&context);
    return result == 0;
}
}

AuthService::AuthService(Config& config, IConfigRepository& repository)
    : config_(config), repository_(repository) {}

Result AuthService::begin() {
    if (config_.auth.username.view().empty() && !config_.auth.username.assign("admin"))
        return Result::Invalid;
    if (!verifyPassword("admin")) {
        if (config_.auth.salted_password_kdf.view().empty()) return setPasswordHash("admin");
        return Result::Accepted; // Existing user supplied a different password.
    }
    return Result::Accepted;
}

bool AuthService::makeSaltedHash(std::string_view password, char* out, std::size_t capacity) {
    if (!out || password.empty() || password.size() > 128 || capacity < 112) return false;
    std::uint8_t salt[kSaltBytes]{}, digest[kDigestBytes]{};
    esp_fill_random(salt, sizeof(salt));
    if (!derive(password, salt, digest)) return false;
    char salt_hex[kSaltBytes * 2 + 1]{}, digest_hex[kDigestBytes * 2 + 1]{};
    encodeHex(salt, sizeof(salt), salt_hex);
    encodeHex(digest, sizeof(digest), digest_hex);
    const int written = std::snprintf(out, capacity, "pbkdf2_sha256$%lu$%s$%s",
        static_cast<unsigned long>(kPbkdf2Iterations), salt_hex, digest_hex);
    return written > 0 && static_cast<std::size_t>(written) < capacity;
}

bool AuthService::verifyHash(std::string_view password, std::string_view encoding) {
    constexpr std::string_view prefix{"pbkdf2_sha256$12000$"};
    if (encoding.size() < prefix.size() || encoding.substr(0, prefix.size()) != prefix ||
        encoding.size() != prefix.size() + 32 + 1 + 64 ||
        encoding[prefix.size() + 32] != '$') return false;
    std::uint8_t salt[kSaltBytes]{}, expected[kDigestBytes]{}, actual[kDigestBytes]{};
    if (!decodeHex(encoding.substr(prefix.size(), 32), salt, sizeof(salt)) ||
        !decodeHex(encoding.substr(prefix.size() + 33), expected, sizeof(expected)) ||
        !derive(password, salt, actual)) return false;
    return constantTimeEqual(actual, expected, sizeof(actual));
}

bool AuthService::verifyPassword(std::string_view password) const {
    return verifyHash(password, config_.auth.salted_password_kdf.view());
}

Result AuthService::setPasswordHash(std::string_view password) {
    char encoded[192]{};
    if (!makeSaltedHash(password, encoded, sizeof(encoded)) ||
        !config_.auth.salted_password_kdf.assign(encoded)) return Result::IoError;
    config_.auth.onboarding_required = false;
    const auto saved = repository_.save(config_);
    if (saved != Result::Accepted) config_.auth.salted_password_kdf.assign("");
    return saved;
}

std::array<std::uint8_t, 32> AuthService::tokenHash(std::string_view token) {
    std::array<std::uint8_t, 32> digest{};
    const auto* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || mbedtls_md(info,
        reinterpret_cast<const unsigned char*>(token.data()), token.size(), digest.data()) != 0)
        digest.fill(0);
    return digest;
}

bool AuthService::randomToken(Text<64>& token) {
    std::uint8_t bytes[32]{};
    char text[65]{};
    esp_fill_random(bytes, sizeof(bytes));
    encodeHex(bytes, sizeof(bytes), text);
    return token.assign(text);
}

Result AuthService::login(std::string_view username, std::string_view password,
                          MonotonicMs now_ms, AuthSession& session) {
    if (username != config_.auth.username.view() || password.empty() || !verifyPassword(password))
        return Result::Forbidden;
    if (!randomToken(session.cookie) || !randomToken(session.csrf)) return Result::IoError;
    session.expires_ms = now_ms + static_cast<MonotonicMs>(config_.auth.session_lifetime_s) * 1000U;
    auto& slot = sessions_[next_session_++ % kSessionCount];
    slot.cookie_hash = tokenHash(session.cookie.view());
    slot.csrf_hash = tokenHash(session.csrf.view());
    slot.csrf_token = session.csrf;
    slot.expires_ms = session.expires_ms;
    slot.occupied = true;
    return Result::Accepted;
}

const AuthService::SessionSlot* AuthService::findSession(std::string_view cookie,
                                                          MonotonicMs now_ms) const {
    if (cookie.size() != 64) return nullptr;
    const auto hash = tokenHash(cookie);
    for (const auto& slot : sessions_)
        if (slot.occupied && now_ms < slot.expires_ms &&
            constantTimeEqual(hash.data(), slot.cookie_hash.data(), hash.size())) return &slot;
    return nullptr;
}

bool AuthService::authorized(std::string_view cookie, MonotonicMs now_ms) const {
    return findSession(cookie, now_ms) != nullptr;
}

bool AuthService::validCsrf(std::string_view cookie, std::string_view csrf,
                            MonotonicMs now_ms) const {
    const auto* slot = findSession(cookie, now_ms);
    if (!slot || csrf.size() != 64) return false;
    const auto hash = tokenHash(csrf);
    return constantTimeEqual(hash.data(), slot->csrf_hash.data(), hash.size());
}

bool AuthService::csrfToken(std::string_view cookie, MonotonicMs now_ms, CsrfToken& out) const {
    const auto* slot = findSession(cookie, now_ms);
    if (!slot) return false;
    out = slot->csrf_token;
    return true;
}

void AuthService::logout(std::string_view cookie) {
    if (cookie.size() != 64) return;
    const auto hash = tokenHash(cookie);
    for (auto& slot : sessions_)
        if (slot.occupied && constantTimeEqual(hash.data(), slot.cookie_hash.data(), hash.size()))
            slot = {};
}

Result AuthService::changePassword(std::string_view username, std::string_view new_password) {
    if (username.empty() || username.size() > 31 || new_password.size() < 6 || new_password.size() > 128)
        return Result::Invalid;
    Text<32> new_username;
    if (!new_username.assign(username)) return Result::Invalid;
    const auto previous_username = config_.auth.username;
    const auto previous_hash = config_.auth.salted_password_kdf;
    config_.auth.username = new_username;
    const auto result = setPasswordHash(new_password);
    if (result != Result::Accepted) {
        config_.auth.username = previous_username;
        config_.auth.salted_password_kdf = previous_hash;
        return result;
    }
    sessions_ = {};
    return Result::Accepted;
}
} // namespace fl
