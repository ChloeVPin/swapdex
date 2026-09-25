#include "profile_store.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <system_error>

#include "platform.hpp"
#include "util.hpp"

namespace swapdex {
namespace {

constexpr std::size_t registry_maximum_bytes = 2U * 1024U * 1024U;
const std::filesystem::perms secret_permissions = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;

ProfileRecord record_from_json(const Json& value) {
    ProfileRecord record;
    record.id = value.at("id").get<std::string>();
    record.label = value.at("label").get<std::string>();
    if (value.contains("email") && value.at("email").is_string()) {
        record.email = value.at("email").get<std::string>();
    }
    record.plan = value.value("plan", "unknown");
    record.authenticated = value.value("authenticated", false);
    if (value.contains("primary_usage") && value.at("primary_usage").is_object()) {
        UsageWindow window;
        window.remaining_percent = json_optional_integer(value.at("primary_usage"), "remaining_percent");
        window.resets_at = json_optional_integer(value.at("primary_usage"), "resets_at");
        window.duration_minutes = json_optional_integer(value.at("primary_usage"), "duration_minutes");
        record.primary_usage = window;
    }
    if (value.contains("secondary_usage") && value.at("secondary_usage").is_object()) {
        UsageWindow window;
        window.remaining_percent = json_optional_integer(value.at("secondary_usage"), "remaining_percent");
        window.resets_at = json_optional_integer(value.at("secondary_usage"), "resets_at");
        window.duration_minutes = json_optional_integer(value.at("secondary_usage"), "duration_minutes");
        record.secondary_usage = window;
    }
    record.lifetime_tokens = json_optional_integer(value, "lifetime_tokens");
    record.credits_balance = json_optional_string(value, "credits_balance");
    record.credits_unlimited = value.value("credits_unlimited", false);
    record.credits_available = value.value("credits_available", false);
    record.available_reset_credits = json_optional_integer(value, "available_reset_credits");
    if (!record.available_reset_credits.has_value()) {
        record.available_reset_credits = json_optional_integer(value, "available_credits");
    }
    if (value.contains("reset_credits") && value.at("reset_credits").is_array()) {
        for (const Json& item : value.at("reset_credits")) {
            if (!item.is_object()) {
                continue;
            }
            ResetCreditDetail detail;
            detail.reset_type = item.value("reset_type", "unknown");
            detail.status = item.value("status", "unknown");
            detail.expires_at = json_optional_integer(item, "expires_at");
            detail.title = item.value("title", "");
            detail.description = item.value("description", "");
            record.reset_credits.push_back(std::move(detail));
        }
    }
    record.usage_updated_at = json_optional_integer(value, "usage_updated_at");
    record.last_maintenance_at = json_optional_integer(value, "last_maintenance_at");
    record.maintenance_status = value.value("maintenance_status", "pending");
    return record;
}

Json usage_to_json(const std::optional<UsageWindow>& usage) {
    if (!usage.has_value()) {
        return Json();
    }
    Json value = Json::object();
    if (usage->remaining_percent.has_value()) {
        value["remaining_percent"] = *usage->remaining_percent;
    }
    if (usage->resets_at.has_value()) {
        value["resets_at"] = *usage->resets_at;
    }
    if (usage->duration_minutes.has_value()) {
        value["duration_minutes"] = *usage->duration_minutes;
    }
    return value;
}

Json record_to_json(const ProfileRecord& record) {
    Json value = Json::object();
    value["id"] = record.id;
    value["label"] = record.label;
    value["email"] = record.email.has_value() ? Json(*record.email) : Json();
    value["plan"] = record.plan;
    value["authenticated"] = record.authenticated;
    value["primary_usage"] = usage_to_json(record.primary_usage);
    value["secondary_usage"] = usage_to_json(record.secondary_usage);
    if (record.lifetime_tokens.has_value()) {
        value["lifetime_tokens"] = *record.lifetime_tokens;
    } else {
        value["lifetime_tokens"] = Json();
    }
    value["credits_balance"] = record.credits_balance.has_value() ? Json(*record.credits_balance) : Json();
    value["credits_unlimited"] = record.credits_unlimited;
    value["credits_available"] = record.credits_available;
    if (record.available_reset_credits.has_value()) {
        value["available_reset_credits"] = *record.available_reset_credits;
    } else {
        value["available_reset_credits"] = Json();
    }
    value["reset_credits"] = Json::array();
    for (const ResetCreditDetail& detail : record.reset_credits) {
        Json item = Json::object();
        item["reset_type"] = detail.reset_type;
        item["status"] = detail.status;
        item["expires_at"] = detail.expires_at.has_value() ? Json(*detail.expires_at) : Json();
        item["title"] = detail.title;
        item["description"] = detail.description;
        value["reset_credits"].push_back(std::move(item));
    }
    if (record.usage_updated_at.has_value()) {
        value["usage_updated_at"] = *record.usage_updated_at;
    } else {
        value["usage_updated_at"] = Json();
    }
    if (record.last_maintenance_at.has_value()) {
        value["last_maintenance_at"] = *record.last_maintenance_at;
    } else {
        value["last_maintenance_at"] = Json();
    }
    value["maintenance_status"] = record.maintenance_status;
    return value;
}

Json registry_to_json(const ProfileRecord* active, const std::vector<ProfileRecord>& profiles) {
    Json value = Json::object();
    value["version"] = 1;
    value["active_id"] = active == nullptr ? "" : active->id;
    value["profiles"] = Json::array();
    for (const auto& record : profiles) {
        value["profiles"].push_back(record_to_json(record));
    }
    return value;
}

bool regular_file_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

bool files_identical(const std::filesystem::path& first, const std::filesystem::path& second) {
    try {
        if (!regular_file_exists(first) || !regular_file_exists(second)) {
            return false;
        }
        std::error_code error;
        const std::uintmax_t first_size = std::filesystem::file_size(first, error);
        if (error) {
            return false;
        }
        const std::uintmax_t second_size = std::filesystem::file_size(second, error);
        if (error || first_size != second_size || first_size > 1024U * 1024U) {
            return false;
        }
        return read_file(first, 1024U * 1024U) == read_file(second, 1024U * 1024U);
    } catch (const std::exception&) {
        return false;
    }
}

}

ProfileStore::ProfileStore(std::filesystem::path state_root, std::filesystem::path shared_codex_home, std::filesystem::path electron_user_data)
    : state_root_(std::move(state_root)),
      shared_codex_home_(std::move(shared_codex_home)),
      electron_user_data_(std::move(electron_user_data)) {}

void ProfileStore::initialize() {
    std::lock_guard lock(mutex_);
    ensure_private_directory(state_root_);
    ensure_private_directory(state_root_ / "profiles");
    ensure_private_directory(state_root_ / "onboarding");
    ensure_private_directory(state_root_ / "backups");
    ensure_private_directory(shared_codex_home_);
    load_locked();
    recover_transaction_locked();
    prune_pending_placeholders_locked();
    prune_orphan_profiles_locked();
    if (registry_.profiles.empty() && regular_file_exists(shared_codex_home_ / "auth.json")) {
        ProfileRecord record;
        record.id = "current-" + random_identifier(6);
        record.label = "Current account";
        record.plan = "unknown";
        record.authenticated = true;
        ensure_private_directory(profile_home(record.id));
        capture_live_auth_locked(record.id);
        registry_.active_id = record.id;
        registry_.profiles.push_back(record);
        save_locked(registry_);
    }
    validate_locked(registry_);
}

void ProfileStore::prune_pending_placeholders() {
    std::lock_guard lock(mutex_);
    prune_pending_placeholders_locked();
}

std::vector<ProfileRecord> ProfileStore::list_existing() {
    std::lock_guard lock(mutex_);
    registry_ = Registry{};
    if (!regular_file_exists(registry_path())) {
        return {};
    }
    if (regular_file_exists(transaction_path())) {
        throw Error("profile_store_busy", "Account data is being updated; try again shortly");
    }
    load_locked();
    return registry_.profiles;
}

std::optional<ProfileRecord> ProfileStore::active_existing() {
    std::lock_guard lock(mutex_);
    registry_ = Registry{};
    if (!regular_file_exists(registry_path())) {
        return std::nullopt;
    }
    if (regular_file_exists(transaction_path())) {
        throw Error("profile_store_busy", "Account data is being updated; try again shortly");
    }
    load_locked();
    const ProfileRecord* record = find_locked(registry_, registry_.active_id);
    return record == nullptr ? std::nullopt : std::optional<ProfileRecord>(*record);
}

std::vector<ProfileRecord> ProfileStore::list() const {
    std::lock_guard lock(mutex_);
    return registry_.profiles;
}

std::optional<ProfileRecord> ProfileStore::active() const {
    std::lock_guard lock(mutex_);
    const ProfileRecord* record = find_locked(registry_, registry_.active_id);
    return record == nullptr ? std::nullopt : std::optional<ProfileRecord>(*record);
}

std::optional<ProfileRecord> ProfileStore::find(std::string_view id) const {
    std::lock_guard lock(mutex_);
    const ProfileRecord* record = find_locked(registry_, std::string(id));
    return record == nullptr ? std::nullopt : std::optional<ProfileRecord>(*record);
}

ProfileRecord ProfileStore::create_pending(std::string_view label) {
    std::lock_guard lock(mutex_);
    if (registry_.profiles.size() >= 32U) {
        throw Error("profile_limit_reached", "The account profile limit has been reached");
    }
    ProfileRecord record;
    record.id = "account-" + random_identifier(8);
    record.label = sanitize_label(label);
    record.plan = "unknown";
    record.authenticated = false;
    ensure_private_directory(profile_home(record.id));
    registry_.profiles.push_back(record);
    save_locked(registry_);
    return record;
}

void ProfileStore::update_metadata(const std::string& id, std::optional<std::string> email, std::string plan, bool authenticated) {
    std::lock_guard lock(mutex_);
    ProfileRecord* record = find_locked(registry_, id);
    if (record == nullptr) {
        throw Error("profile_not_found", "The requested account profile does not exist");
    }
    record->email = std::move(email);
    record->plan = plan.empty() ? "unknown" : std::move(plan);
    record->authenticated = authenticated;
    save_locked(registry_);
}

void ProfileStore::update_usage(const std::string& id, std::optional<UsageWindow> primary, std::optional<UsageWindow> secondary, std::optional<std::int64_t> lifetime_tokens, std::optional<std::string> credits_balance, bool credits_unlimited, bool credits_available, std::optional<std::int64_t> available_reset_credits, std::vector<ResetCreditDetail> reset_credits) {
    std::lock_guard lock(mutex_);
    ProfileRecord* record = find_locked(registry_, id);
    if (record == nullptr) {
        throw Error("profile_not_found", "The requested account profile does not exist");
    }
    record->primary_usage = primary;
    record->secondary_usage = secondary;
    record->lifetime_tokens = lifetime_tokens;
    record->credits_balance = std::move(credits_balance);
    record->credits_unlimited = credits_unlimited;
    record->credits_available = credits_available;
    record->available_reset_credits = available_reset_credits;
    record->reset_credits = std::move(reset_credits);
    record->usage_updated_at = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    save_locked(registry_);
}

void ProfileStore::update_maintenance(const std::string& id, std::string status) {
    std::lock_guard lock(mutex_);
    ProfileRecord* record = find_locked(registry_, id);
    if (record == nullptr) {
        throw Error("profile_not_found", "The requested account profile does not exist");
    }
    record->maintenance_status = status == "ok" ? "ok" : "error";
    record->last_maintenance_at = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    save_locked(registry_);
}

RemovalResult ProfileStore::remove(std::string_view id) {
    std::lock_guard lock(mutex_);
    if (!valid_profile_id(id)) {
        throw Error("profile_id_invalid", "The account profile identifier is invalid");
    }
    const std::string target(id);
    const ProfileRecord* record = find_locked(registry_, target);
    if (record == nullptr) {
        throw Error("profile_not_found", "The requested account profile does not exist");
    }
    if (regular_file_exists(transaction_path())) {
        throw Error("profile_store_busy", "Account data is being updated; try again shortly");
    }
    RemovalResult result;
    result.removed_label = record->label.empty() ? record->id : record->label;
    result.removed_active = registry_.active_id == target;
    std::string replacement_id;
    if (result.removed_active) {
        for (const ProfileRecord& candidate : registry_.profiles) {
            if (candidate.id != target && candidate.authenticated && regular_file_exists(profile_auth(candidate.id))) {
                replacement_id = candidate.id;
                break;
            }
        }
    }
    result.replacement_active = replacement_id.empty() ? std::nullopt : std::optional<std::string>(replacement_id);
    Json transaction = Json::object();
    transaction["version"] = 1;
    transaction["operation"] = "remove";
    transaction["from_id"] = target;
    transaction["to_id"] = replacement_id;
    transaction["had_live_auth"] = regular_file_exists(shared_codex_home_ / "auth.json");
    write_json_file_atomically(transaction_path(), transaction, secret_permissions);
    complete_removal_locked(target, replacement_id);
    remove_file_durable(transaction_path());
    return result;
}

void ProfileStore::clear_live_auth() {
    std::lock_guard lock(mutex_);
    remove_live_auth_locked();
}

void ProfileStore::capture_live_auth(const std::string& id) {
    std::lock_guard lock(mutex_);
    capture_live_auth_locked(id);
    ProfileRecord* record = find_locked(registry_, id);
    if (record != nullptr) {
        record->authenticated = true;
        save_locked(registry_);
    }
}

void ProfileStore::restore_live_auth(const std::string& id) {
    std::lock_guard lock(mutex_);
    restore_live_auth_locked(id);
}

void ProfileStore::activate(const std::string& id) {
    std::lock_guard lock(mutex_);
    ProfileRecord* target = find_locked(registry_, id);
    if (target == nullptr) {
        throw Error("profile_not_found", "The requested account profile does not exist");
    }
    if (!regular_file_exists(profile_auth(id))) {
        throw Error("profile_auth_missing", "The selected account has not completed sign-in");
    }
    const std::string from_id = registry_.active_id;
    const bool had_live_auth = regular_file_exists(shared_codex_home_ / "auth.json");
    if (had_live_auth) {
        copy_file_atomically(shared_codex_home_ / "auth.json", auth_backup_path(), secret_permissions);
    }
    Json transaction = Json::object();
    transaction["version"] = 1;
    transaction["from_id"] = from_id;
    transaction["to_id"] = id;
    transaction["had_live_auth"] = had_live_auth;
    write_json_file_atomically(transaction_path(), transaction, secret_permissions);
    bool registry_saved = false;
    try {
        restore_live_auth_locked(id);
        registry_.active_id = id;
        save_locked(registry_);
        registry_saved = true;
        remove_file_durable(transaction_path());
    } catch (...) {
        registry_.active_id = registry_saved ? id : from_id;
        try {
            recover_transaction_locked();
        } catch (const Error&) {
        }
        throw;
    }
}

std::filesystem::path ProfileStore::profile_home(const std::string& id) const {
    if (!valid_profile_id(id)) {
        throw Error("profile_id_invalid", "The account profile identifier is invalid");
    }
    return state_root_ / "profiles" / id;
}

std::filesystem::path ProfileStore::profile_auth(const std::string& id) const {
    return profile_home(id) / "auth.json";
}

std::filesystem::path ProfileStore::root() const {
    return state_root_;
}

std::filesystem::path ProfileStore::shared_codex_home() const {
    return shared_codex_home_;
}

std::filesystem::path ProfileStore::electron_user_data() const {
    return electron_user_data_;
}

std::filesystem::path ProfileStore::onboarding_user_data(const std::string& id) const {
    if (!valid_profile_id(id)) {
        throw Error("profile_id_invalid", "The account profile identifier is invalid");
    }
    return state_root_ / "onboarding" / id;
}

void ProfileStore::set_singleton_lock(std::shared_ptr<platform::InstanceLock> guard) {
    std::lock_guard<std::mutex> lock(mutex_);
    singleton_lock_ = std::move(guard);
}

void ProfileStore::load_locked() {
    if (!regular_file_exists(registry_path())) {
        registry_ = Registry{};
        return;
    }
    const Json value = read_json_file(registry_path(), registry_maximum_bytes);
    if (!value.is_object() || value.value("version", 0) != 1 || !value.contains("active_id") || !value.at("active_id").is_string() || !value.contains("profiles") || !value.at("profiles").is_array()) {
        throw Error("registry_invalid", "The account registry is invalid");
    }
    registry_.active_id = value.at("active_id").get<std::string>();
    registry_.profiles.clear();
    for (const Json& item : value.at("profiles")) {
        registry_.profiles.push_back(record_from_json(item));
    }
    validate_locked(registry_);
}

void ProfileStore::save_locked(const Registry& registry) {
    const ProfileRecord* active = find_locked(registry, registry.active_id);
    write_json_file_atomically(registry_path(), registry_to_json(active, registry.profiles), secret_permissions);
}

ProfileRecord* ProfileStore::find_locked(Registry& registry, const std::string& id) {
    for (ProfileRecord& record : registry.profiles) {
        if (record.id == id) {
            return &record;
        }
    }
    return nullptr;
}

const ProfileRecord* ProfileStore::find_locked(const Registry& registry, const std::string& id) const {
    for (const ProfileRecord& record : registry.profiles) {
        if (record.id == id) {
            return &record;
        }
    }
    return nullptr;
}

void ProfileStore::validate_locked(const Registry& registry) const {
    std::vector<std::string> identifiers;
    identifiers.reserve(registry.profiles.size());
    if (registry.profiles.size() > 32U) {
        throw Error("registry_invalid", "The account registry contains too many profiles");
    }
    for (const ProfileRecord& record : registry.profiles) {
        if (!valid_profile_id(record.id) || sanitize_label(record.label) != record.label || record.plan.size() > 64U || (record.email.has_value() && record.email->size() > 320U) || (record.maintenance_status != "pending" && record.maintenance_status != "ok" && record.maintenance_status != "error")) {
            throw Error("registry_invalid", "The account registry contains an invalid profile");
        }
        identifiers.push_back(record.id);
    }
    std::sort(identifiers.begin(), identifiers.end());
    if (std::adjacent_find(identifiers.begin(), identifiers.end()) != identifiers.end()) {
        throw Error("registry_invalid", "The account registry contains duplicate profiles");
    }
    if (registry.profiles.empty()) {
        if (!registry.active_id.empty()) {
            throw Error("registry_invalid", "The active account profile does not exist");
        }
        return;
    }
    if (!registry.active_id.empty() && find_locked(registry, registry.active_id) == nullptr) {
        throw Error("registry_invalid", "The active account profile does not exist");
    }
}

std::filesystem::path ProfileStore::registry_path() const {
    return state_root_ / "registry.json";
}

std::filesystem::path ProfileStore::auth_backup_path() const {
    return state_root_ / "backups" / "active-auth.json";
}

std::filesystem::path ProfileStore::transaction_path() const {
    return state_root_ / "transaction.json";
}

void ProfileStore::prune_pending_placeholders_locked() {
    std::vector<ProfileRecord> retained;
    retained.reserve(registry_.profiles.size());
    bool changed = false;
    for (const ProfileRecord& record : registry_.profiles) {
        const bool placeholder = !record.authenticated && !record.email.has_value() && record.label == "New account" && record.id != registry_.active_id && !regular_file_exists(profile_auth(record.id));
        if (placeholder) {
            std::error_code error;
            std::filesystem::remove_all(profile_home(record.id), error);
            if (!error) {
                changed = true;
                continue;
            }
        }
        retained.push_back(record);
    }
    if (changed) {
        registry_.profiles = std::move(retained);
        save_locked(registry_);
    }
}

void ProfileStore::prune_orphan_profiles_locked() {
    const std::filesystem::path root = state_root_ / "profiles";
    std::error_code error;
    if (!std::filesystem::is_directory(root, error) || error) {
        return;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, error)) {
        if (error) {
            return;
        }
        std::error_code entry_error;
        if (!entry.is_directory(entry_error) || entry_error) {
            continue;
        }
        if (find_locked(registry_, entry.path().filename().string()) != nullptr) {
            continue;
        }
        std::filesystem::remove_all(entry.path(), entry_error);
        if (entry_error) {
            return;
        }
    }
}

void ProfileStore::remove_live_auth_locked() {
    ensure_private_directory(shared_codex_home_);
    const std::filesystem::path path = shared_codex_home_ / "auth.json";
    if (!regular_file_exists(path)) {
        return;
    }
    remove_file_durable(path);
}

void ProfileStore::remove_profile_home_locked(const std::string& id) {
    if (!valid_profile_id(id)) {
        return;
    }
    std::error_code error;
    std::filesystem::remove_all(profile_home(id), error);
    if (error) {
        throw Error("profile_remove_failed", "Stored account credentials could not be removed");
    }
}

void ProfileStore::discard_backup_for_locked(const std::string& id) {
    const std::filesystem::path backup = auth_backup_path();
    if (!regular_file_exists(backup)) {
        return;
    }
    const bool belongs_to_removed = files_identical(backup, profile_auth(id));
    bool belongs_to_remaining = false;
    if (!belongs_to_removed) {
        for (const ProfileRecord& record : registry_.profiles) {
            if (files_identical(backup, profile_auth(record.id))) {
                belongs_to_remaining = true;
                break;
            }
        }
    }
    if (!belongs_to_remaining) {
        remove_file_durable(backup);
    }
}

void ProfileStore::complete_removal_locked(const std::string& id, const std::string& replacement_id) {
    const bool was_active = registry_.active_id == id;
    const bool live_auth_present = regular_file_exists(shared_codex_home_ / "auth.json");
    if (was_active) {
        if (!replacement_id.empty() && find_locked(registry_, replacement_id) != nullptr && regular_file_exists(profile_auth(replacement_id))) {
            restore_live_auth_locked(replacement_id);
        } else if (live_auth_present) {
            remove_live_auth_locked();
        }
    }
    registry_.profiles.erase(std::remove_if(registry_.profiles.begin(), registry_.profiles.end(), [&id](const ProfileRecord& record) { return record.id == id; }), registry_.profiles.end());
    if (was_active) {
        registry_.active_id = replacement_id;
    }
    validate_locked(registry_);
    save_locked(registry_);
    discard_backup_for_locked(id);
    remove_profile_home_locked(id);
    prune_orphan_profiles_locked();
}

void ProfileStore::recover_transaction_locked() {
    if (!regular_file_exists(transaction_path())) {
        return;
    }
    const Json transaction = read_json_file(transaction_path(), registry_maximum_bytes);
    if (!transaction.is_object() || transaction.value("version", 0) != 1 || !transaction.contains("from_id") || !transaction.at("from_id").is_string() || !transaction.contains("to_id") || !transaction.at("to_id").is_string() || !transaction.contains("had_live_auth") || !transaction.at("had_live_auth").is_boolean()) {
        throw Error("transaction_invalid", "The interrupted account switch journal is invalid");
    }
    const std::string from_id = transaction.at("from_id").get<std::string>();
    const std::string to_id = transaction.at("to_id").get<std::string>();
    const bool had_live_auth = transaction.at("had_live_auth").get<bool>();
    const std::string operation = transaction.value("operation", std::string("switch"));
    if (operation == "remove") {
        complete_removal_locked(from_id, to_id);
        remove_file_durable(transaction_path());
        return;
    }
    if (operation != "switch") {
        throw Error("transaction_invalid", "The interrupted account switch journal is invalid");
    }
    if (registry_.active_id == to_id) {
        remove_file_durable(transaction_path());
        return;
    }
    if (registry_.active_id != from_id) {
        throw Error("transaction_ambiguous", "The interrupted account switch cannot be recovered safely");
    }
    if (had_live_auth) {
        if (!regular_file_exists(auth_backup_path())) {
            throw Error("transaction_backup_missing", "The interrupted account switch backup is missing");
        }
        copy_file_atomically(auth_backup_path(), shared_codex_home_ / "auth.json", secret_permissions);
    } else {
        std::error_code error;
        std::filesystem::remove(shared_codex_home_ / "auth.json", error);
        if (error) {
            throw Error("transaction_rollback_failed", "The interrupted account switch could not be rolled back");
        }
    }
    remove_file_durable(transaction_path());
}

void ProfileStore::remove_file_durable(const std::filesystem::path& path) {
    try {
        platform::remove_file_durable(path);
    } catch (const std::exception&) {
        throw Error("file_remove_failed", "Unable to remove a Swapdex file");
    }
}

void ProfileStore::capture_live_auth_locked(const std::string& id) {
    ensure_private_directory(profile_home(id));
    copy_file_atomically(shared_codex_home_ / "auth.json", profile_auth(id), secret_permissions);
}

void ProfileStore::restore_live_auth_locked(const std::string& id) {
    ensure_private_directory(shared_codex_home_);
    copy_file_atomically(profile_auth(id), shared_codex_home_ / "auth.json", secret_permissions);
}

}
