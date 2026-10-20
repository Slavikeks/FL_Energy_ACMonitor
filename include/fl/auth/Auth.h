#pragma once
#include "fl/core/Config.h"

namespace fl {
using SessionToken = Text<64>;
using CsrfToken = Text<64>;
struct AuthSession {
    SessionToken cookie{};
    CsrfToken csrf{};
    MonotonicMs expires_ms{0};
}; // Auth-private; never StateStore, telemetry, SNMP or logs.
class IAuthService {
public:
    virtual ~IAuthService() = default;
    virtual Result login(std::string_view username, std::string_view password,
                         MonotonicMs, AuthSession&) = 0;
    virtual bool authorized(std::string_view cookie, MonotonicMs) const = 0;
    virtual bool validCsrf(std::string_view cookie, std::string_view csrf,
                           MonotonicMs) const = 0;
    virtual bool csrfToken(std::string_view cookie, MonotonicMs, CsrfToken& out) const = 0;
    virtual void logout(std::string_view cookie) = 0;
    virtual Result changePassword(std::string_view username, std::string_view new_password) = 0;
};
} // namespace fl
