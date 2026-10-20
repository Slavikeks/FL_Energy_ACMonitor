#pragma once
#include "fl/auth/Auth.h"
#include "fl/core/ConfigRepository.h"
#include <array>

namespace fl {
class AuthService final : public IAuthService {
public:
    AuthService(Config& config, IConfigRepository& repository);
    Result begin();
    Result login(std::string_view username, std::string_view password,
                 MonotonicMs now_ms, AuthSession& session) override;
    bool authorized(std::string_view cookie, MonotonicMs now_ms) const override;
    bool validCsrf(std::string_view cookie, std::string_view csrf,
                   MonotonicMs now_ms) const override;
    bool csrfToken(std::string_view cookie, MonotonicMs now_ms, CsrfToken& out) const override;
    void logout(std::string_view cookie) override;
    Result changePassword(std::string_view username, std::string_view new_password) override;
private:
    struct SessionSlot {
        std::array<std::uint8_t, 32> cookie_hash{};
        std::array<std::uint8_t, 32> csrf_hash{};
        CsrfToken csrf_token{};
        MonotonicMs expires_ms{0};
        bool occupied{false};
    };
    static constexpr std::size_t kSessionCount = 8;
    Config& config_;
    IConfigRepository& repository_;
    std::array<SessionSlot, kSessionCount> sessions_{};
    std::size_t next_session_{0};
    Result setPasswordHash(std::string_view password);
    bool verifyPassword(std::string_view password) const;
    static bool makeSaltedHash(std::string_view password, char* out, std::size_t capacity);
    static bool verifyHash(std::string_view password, std::string_view encoding);
    static bool randomToken(Text<64>& token);
    static std::array<std::uint8_t, 32> tokenHash(std::string_view token);
    const SessionSlot* findSession(std::string_view cookie, MonotonicMs now_ms) const;
};
} // namespace fl
