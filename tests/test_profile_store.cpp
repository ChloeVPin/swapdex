#include <filesystem>
#include <string>
#include <sys/stat.h>

#include "profile_store.hpp"
#include "test.hpp"
#include "util.hpp"

namespace {

std::filesystem::path test_root() {
    return std::filesystem::temp_directory_path() / ("swapdex-profile-test-" + swapdex::random_identifier(8));
}

mode_t permissions_of(const std::filesystem::path& path) {
    struct stat status {};
    if (stat(path.c_str(), &status) != 0) {
        throw swapdex::Error("test_stat_failed", "Unable to inspect test permissions");
    }
    return status.st_mode & 0777;
}

}

void test_profile_store() {
    const std::filesystem::path root = test_root();
    const std::filesystem::path state = root / "state";
    const std::filesystem::path codex = root / "codex";
    const std::filesystem::path electron = root / "electron";
    swapdex::ensure_private_directory(codex);
    swapdex::ensure_private_directory(electron);
    swapdex::write_file_atomically(codex / "auth.json", "{\"tokens\":{\"test\":\"first\"}}\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    swapdex::ProfileStore store(state, codex, electron);
    store.initialize();
    const auto initial = store.active();
    swapdex::test::check(initial.has_value(), "Initial profile was not created");
    swapdex::test::check(initial->authenticated, "Initial profile was not authenticated");
    const std::string copied_auth = swapdex::read_file(store.profile_auth(initial->id), 4096);
    if (copied_auth != "{\"tokens\":{\"test\":\"first\"}}\n") {
        throw swapdex::test::Failure("Initial auth copy changed: " + copied_auth);
    }
    swapdex::test::check_equal(permissions_of(store.profile_auth(initial->id)), static_cast<mode_t>(0600), "Profile auth permissions are not 0600");
    swapdex::test::check_equal(permissions_of(state / "registry.json"), static_cast<mode_t>(0600), "Registry permissions are not 0600");
    swapdex::test::check_equal(permissions_of(state), static_cast<mode_t>(0700), "State directory permissions are not 0700");
    store.update_maintenance(initial->id, "ok");
    swapdex::test::check_equal(store.active()->maintenance_status, std::string("ok"), "Maintenance status was not recorded");
    swapdex::test::check(store.active()->last_maintenance_at.has_value(), "Maintenance timestamp was not recorded");
    swapdex::UsageWindow primary_usage;
    primary_usage.remaining_percent = 75;
    swapdex::UsageWindow secondary_usage;
    secondary_usage.remaining_percent = 50;
    std::vector<swapdex::ResetCreditDetail> reset_credits;
    reset_credits.push_back({"codexRateLimits", "available", 1893456000, "Full reset", "Restores your limits"});
    store.update_usage(initial->id, primary_usage, secondary_usage, 123, "125.50", false, true, 3, reset_credits);
    swapdex::test::check_equal(*store.active()->credits_balance, std::string("125.50"), "General credits were not recorded");
    swapdex::test::check(store.active()->credits_available, "General credit availability was not recorded");
    swapdex::test::check_equal(*store.active()->available_reset_credits, static_cast<std::int64_t>(3), "Available reset credits were not recorded");
    swapdex::ProfileStore status_store(state, codex, electron);
    status_store.initialize();
    swapdex::test::check_equal(status_store.find(initial->id)->maintenance_status, std::string("ok"), "Maintenance status was not persisted");
    swapdex::test::check_equal(*status_store.find(initial->id)->credits_balance, std::string("125.50"), "General credits were not persisted");
    swapdex::test::check(status_store.find(initial->id)->credits_available, "General credit availability was not persisted");
    swapdex::test::check_equal(*status_store.find(initial->id)->available_reset_credits, static_cast<std::int64_t>(3), "Available reset credits were not persisted");
    swapdex::test::check_equal(status_store.find(initial->id)->reset_credits.size(), static_cast<std::size_t>(1), "Reset-credit details were not persisted");
    swapdex::test::check_equal(*status_store.find(initial->id)->reset_credits.front().expires_at, static_cast<std::int64_t>(1893456000), "Reset-credit expiry was not persisted");
    swapdex::ProfileStore read_only_store(state, codex, electron);
    const auto existing_profiles = read_only_store.list_existing();
    const auto existing_active = read_only_store.active_existing();
    swapdex::test::check_equal(existing_profiles.size(), static_cast<std::size_t>(1), "Read-only profile listing returned the wrong count");
    swapdex::test::check(existing_active.has_value(), "Read-only active profile lookup failed");
    const swapdex::ProfileRecord placeholder = store.create_pending("New account");
    store.prune_pending_placeholders();
    swapdex::test::check(!store.find(placeholder.id).has_value(), "A canceled placeholder account was not removed");
    const swapdex::ProfileRecord second = store.create_pending("Work");
    swapdex::write_file_atomically(store.profile_auth(second.id), "{\"tokens\":{\"test\":\"second\"}}\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    store.update_metadata(second.id, "work@example.test", "pro", true);
    store.activate(second.id);
    swapdex::test::check_equal(store.active()->id, second.id, "Active profile did not change");
    swapdex::test::check_equal(swapdex::read_file(codex / "auth.json", 4096), std::string("{\"tokens\":{\"test\":\"second\"}}\n"), "Live auth was not replaced");
    store.capture_live_auth(initial->id);
    swapdex::test::check_equal(swapdex::read_file(store.profile_auth(initial->id), 4096), std::string("{\"tokens\":{\"test\":\"second\"}}\n"), "Live auth was not captured before rollback");
    const auto secret_permissions = std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
    swapdex::copy_file_atomically(codex / "auth.json", state / "backups" / "active-auth.json", secret_permissions);
    swapdex::Json rollback_journal = {{"version", 1}, {"from_id", second.id}, {"to_id", initial->id}, {"had_live_auth", true}};
    swapdex::write_json_file_atomically(state / "transaction.json", rollback_journal, secret_permissions);
    swapdex::write_file_atomically(codex / "auth.json", "{\"tokens\":{\"test\":\"interrupted\"}}\n", secret_permissions);
    swapdex::ProfileStore rollback_store(state, codex, electron);
    rollback_store.initialize();
    swapdex::test::check_equal(rollback_store.active()->id, second.id, "Interrupted switch changed the active profile during rollback");
    swapdex::test::check_equal(swapdex::read_file(codex / "auth.json", 4096), std::string("{\"tokens\":{\"test\":\"second\"}}\n"), "Interrupted switch did not restore live auth");
    swapdex::test::check(!std::filesystem::exists(state / "transaction.json"), "Interrupted switch journal was not removed");
    store.activate(initial->id);
    swapdex::copy_file_atomically(codex / "auth.json", state / "backups" / "active-auth.json", secret_permissions);
    swapdex::Json commit_journal = {{"version", 1}, {"from_id", second.id}, {"to_id", initial->id}, {"had_live_auth", true}};
    swapdex::write_json_file_atomically(state / "transaction.json", commit_journal, secret_permissions);
    swapdex::ProfileStore commit_store(state, codex, electron);
    commit_store.initialize();
    swapdex::test::check_equal(commit_store.active()->id, initial->id, "Committed switch lost the active profile");
    swapdex::test::check(!std::filesystem::exists(state / "transaction.json"), "Committed switch journal was not removed");
    bool rejected = false;
    try {
        store.activate("missing-profile");
    } catch (const swapdex::Error&) {
        rejected = true;
    }
    swapdex::test::check(rejected, "Unknown profile was accepted");
    rejected = false;
    try {
        store.remove("../escape");
    } catch (const swapdex::Error&) {
        rejected = true;
    }
    swapdex::test::check(rejected, "An invalid profile identifier was accepted for removal");
    rejected = false;
    try {
        store.remove("missing-profile");
    } catch (const swapdex::Error&) {
        rejected = true;
    }
    swapdex::test::check(rejected, "Removing an unknown profile was accepted");
    const std::string live_before_secondary = swapdex::read_file(codex / "auth.json", 4096);
    const swapdex::RemovalResult secondary = store.remove(second.id);
    swapdex::test::check(!secondary.removed_active, "Removing a secondary account reported it as active");
    swapdex::test::check(!store.find(second.id).has_value(), "The removed secondary account is still stored");
    swapdex::test::check(!std::filesystem::exists(store.profile_home(second.id)), "The removed account credentials were not deleted");
    swapdex::test::check_equal(store.active()->id, initial->id, "Removing a secondary account changed the active account");
    swapdex::test::check_equal(swapdex::read_file(codex / "auth.json", 4096), live_before_secondary, "Removing a secondary account changed the live credentials");
    swapdex::test::check(!std::filesystem::exists(state / "transaction.json"), "The removal journal was not cleared");
    const swapdex::RemovalResult last = store.remove(initial->id);
    swapdex::test::check(last.removed_active, "Removing the signed-in account did not report it as active");
    swapdex::test::check(!last.replacement_active.has_value(), "A removed account reported a replacement that does not exist");
    swapdex::test::check(store.list().empty(), "The registry still lists the removed account");
    swapdex::test::check(!store.active().has_value(), "An active account remains after removing the last one");
    swapdex::test::check(!std::filesystem::exists(codex / "auth.json"), "Live credentials survived removing the signed-in account");
    swapdex::test::check(!std::filesystem::exists(state / "backups" / "active-auth.json"), "A stale credential backup survived removal");
    swapdex::ProfileStore signed_out_store(state, codex, electron);
    signed_out_store.initialize();
    swapdex::test::check(signed_out_store.list().empty(), "Removal was not persisted");
    swapdex::test::check(!signed_out_store.active().has_value(), "A signed-out registry reported an active account");
    const swapdex::ProfileRecord promoted_source = store.create_pending("First");
    swapdex::write_file_atomically(store.profile_auth(promoted_source.id), "{\"tokens\":{\"test\":\"first-promoted\"}}\n", secret_permissions);
    store.update_metadata(promoted_source.id, "first@example.test", "plus", true);
    const swapdex::ProfileRecord promoted_target = store.create_pending("Second");
    swapdex::write_file_atomically(store.profile_auth(promoted_target.id), "{\"tokens\":{\"test\":\"second-promoted\"}}\n", secret_permissions);
    store.update_metadata(promoted_target.id, "second@example.test", "pro", true);
    store.activate(promoted_source.id);
    swapdex::test::check_equal(swapdex::read_file(codex / "auth.json", 4096), std::string("{\"tokens\":{\"test\":\"first-promoted\"}}\n"), "Promotion setup did not activate the source account");
    const swapdex::RemovalResult promoted = store.remove(promoted_source.id);
    swapdex::test::check(promoted.removed_active, "Removing the signed-in account did not report it as active");
    swapdex::test::check(promoted.replacement_active.has_value(), "Removing the signed-in account did not promote a stored account");
    if (promoted.replacement_active.has_value()) {
        swapdex::test::check_equal(*promoted.replacement_active, promoted_target.id, "The wrong account was promoted");
    }
    swapdex::test::check_equal(store.active()->id, promoted_target.id, "The promoted account did not become active");
    swapdex::test::check_equal(swapdex::read_file(codex / "auth.json", 4096), std::string("{\"tokens\":{\"test\":\"second-promoted\"}}\n"), "The promoted account credentials were not made live");
    swapdex::test::check(!std::filesystem::exists(store.profile_home(promoted_source.id)), "The removed signed-in account kept its credentials");
    swapdex::test::check(std::filesystem::exists(store.profile_auth(promoted_target.id)), "The promoted account lost its credentials");
    swapdex::test::check_equal(store.list().size(), std::size_t(1), "The registry kept the removed signed-in account");
    const swapdex::ProfileRecord interrupted = store.create_pending("Interrupted");
    swapdex::write_file_atomically(store.profile_auth(interrupted.id), "{\"tokens\":{\"test\":\"interrupted-remove\"}}\n", secret_permissions);
    store.update_metadata(interrupted.id, "interrupted@example.test", "plus", true);
    swapdex::test::check_equal(store.active()->id, promoted_target.id, "Adding an account changed the active account");
    swapdex::Json remove_journal = {{"version", 1}, {"operation", "remove"}, {"from_id", interrupted.id}, {"to_id", ""}, {"had_live_auth", true}};
    swapdex::write_json_file_atomically(state / "transaction.json", remove_journal, secret_permissions);
    swapdex::ProfileStore interrupted_store(state, codex, electron);
    interrupted_store.initialize();
    swapdex::test::check(!interrupted_store.find(interrupted.id).has_value(), "An interrupted removal was rolled back instead of completed");
    swapdex::test::check(!std::filesystem::exists(interrupted_store.profile_home(interrupted.id)), "An interrupted removal kept stored credentials");
    swapdex::test::check_equal(interrupted_store.active()->id, promoted_target.id, "An interrupted removal changed the active account");
    swapdex::test::check_equal(swapdex::read_file(codex / "auth.json", 4096), std::string("{\"tokens\":{\"test\":\"second-promoted\"}}\n"), "An interrupted removal changed the live credentials");
    swapdex::test::check(!std::filesystem::exists(state / "transaction.json"), "The interrupted removal journal was not cleared");
    std::error_code error;
    std::filesystem::remove_all(root, error);
}
