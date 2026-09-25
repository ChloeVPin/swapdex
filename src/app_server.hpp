#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

#include "profile_store.hpp"

namespace swapdex {

struct AppServerSnapshot {
    std::optional<std::string> email;
    std::string plan;
    std::optional<UsageWindow> primary_usage;
    std::optional<UsageWindow> secondary_usage;
    std::optional<std::int64_t> lifetime_tokens;
    std::optional<std::string> credits_balance;
    bool credits_unlimited = false;
    bool credits_available = false;
    std::optional<std::int64_t> available_reset_credits;
    std::vector<ResetCreditDetail> reset_credits;
    std::string account_id;
    std::int64_t observed_at = 0;
};

class AppServerClient {
public:
    explicit AppServerClient(std::filesystem::path executable = SWAPDEX_CODEX_BINARY);

    AppServerSnapshot query(const std::filesystem::path& codex_home, std::chrono::milliseconds timeout = std::chrono::seconds(60), bool proactive_token_refresh = false);

private:
    std::filesystem::path executable_;
};

}
