#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace swapdex {

struct UsageWindow {
    std::optional<int> remaining_percent;
    std::optional<std::int64_t> resets_at;
    std::optional<std::int64_t> duration_minutes;
};

struct ResetCreditDetail {
    std::string reset_type;
    std::string status;
    std::optional<std::int64_t> expires_at;
    std::string title;
    std::string description;
};

struct ProfileRecord {
    std::string id;
    std::string label;
    std::optional<std::string> email;
    std::string plan;
    bool authenticated = false;
    std::optional<UsageWindow> primary_usage;
    std::optional<UsageWindow> secondary_usage;
    std::optional<std::int64_t> lifetime_tokens;
    std::optional<std::string> credits_balance;
    bool credits_unlimited = false;
    bool credits_available = false;
    std::optional<std::int64_t> available_reset_credits;
    std::vector<ResetCreditDetail> reset_credits;
    std::optional<std::int64_t> usage_updated_at;
    std::optional<std::int64_t> last_maintenance_at;
    std::string maintenance_status = "pending";
};

struct RemovalResult {
    bool removed_active = false;
    std::optional<std::string> replacement_active;
    std::string removed_label;
};

class ProfileStore {
public:
    ProfileStore(std::filesystem::path state_root, std::filesystem::path shared_codex_home, std::filesystem::path electron_user_data);

    void initialize();
    std::vector<ProfileRecord> list_existing();
    std::optional<ProfileRecord> active_existing();
    std::vector<ProfileRecord> list() const;
    std::optional<ProfileRecord> active() const;
    std::optional<ProfileRecord> find(std::string_view id) const;
    ProfileRecord create_pending(std::string_view label);
    void update_metadata(const std::string& id, std::optional<std::string> email, std::string plan, bool authenticated);
    void update_usage(const std::string& id, std::optional<UsageWindow> primary, std::optional<UsageWindow> secondary, std::optional<std::int64_t> lifetime_tokens, std::optional<std::string> credits_balance, bool credits_unlimited, bool credits_available, std::optional<std::int64_t> available_reset_credits, std::vector<ResetCreditDetail> reset_credits);
    void update_maintenance(const std::string& id, std::string status);
    RemovalResult remove(std::string_view id);
    void clear_live_auth();
    void prune_pending_placeholders();
    void capture_live_auth(const std::string& id);
    void restore_live_auth(const std::string& id);
    void activate(const std::string& id);
    std::filesystem::path profile_home(const std::string& id) const;
    std::filesystem::path profile_auth(const std::string& id) const;
    std::filesystem::path root() const;
    std::filesystem::path shared_codex_home() const;
    std::filesystem::path electron_user_data() const;
    std::filesystem::path onboarding_user_data(const std::string& id) const;
    void set_singleton_lock(int fd);

private:
    struct Registry {
        int version = 1;
        std::string active_id;
        std::vector<ProfileRecord> profiles;
    };

    void load_locked();
    void save_locked(const Registry& registry);
    ProfileRecord* find_locked(Registry& registry, const std::string& id);
    const ProfileRecord* find_locked(const Registry& registry, const std::string& id) const;
    void validate_locked(const Registry& registry) const;
    std::filesystem::path registry_path() const;
    std::filesystem::path auth_backup_path() const;
    std::filesystem::path transaction_path() const;
    void recover_transaction_locked();
    void prune_pending_placeholders_locked();
    void prune_orphan_profiles_locked();
    void complete_removal_locked(const std::string& id, const std::string& replacement_id);
    void remove_live_auth_locked();
    void remove_profile_home_locked(const std::string& id);
    void discard_backup_for_locked(const std::string& id);
    void remove_file_durable(const std::filesystem::path& path);
    void capture_live_auth_locked(const std::string& id);
    void restore_live_auth_locked(const std::string& id);

    std::filesystem::path state_root_;
    std::filesystem::path shared_codex_home_;
    std::filesystem::path electron_user_data_;
    mutable std::mutex mutex_;
    Registry registry_;
    int singleton_fd_ = -1;
};

}
