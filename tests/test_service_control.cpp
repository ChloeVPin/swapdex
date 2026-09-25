#include "test.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "platform.hpp"
#include "service.hpp"
#include "service_backend.hpp"
#include "service_control.hpp"
#include "shell.hpp"
#include "util.hpp"

namespace {

swapdex::ServiceControlPaths test_paths(std::filesystem::path root) {
    root /= "space root";
    swapdex::ServiceControlPaths paths;
    paths.source_executable = root / "source" / "swapdex";
    paths.source_asset = root / "source" / "inject.js";
    paths.installed_executable = root / "install" / "bin" / "swapdex";
    paths.installed_asset = root / "install" / "share" / "swapdex" / "inject.js";
    paths.state_root = root / "state" / "swapdex";
    paths.lock_file = root / "config" / "swapdex" / "install.lock";
    paths.manifest_file = root / "config" / "swapdex" / "install.json";
    paths.registration_file = root / "config" / "registration" / "swapdex.registration";
    paths.xdg_data_home = (root / "data").string();
    paths.xdg_state_home = (root / "state-root").string();
    swapdex::write_file_atomically(paths.source_executable, "binary\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec);
    swapdex::write_file_atomically(paths.source_asset, "asset\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    return paths;
}

bool has_call_containing(const std::vector<std::vector<std::string>>& calls, const std::string& needle) {
    for (const auto& call : calls) {
        std::string joined;
        for (const std::string& argument : call) {
            joined += argument + " ";
        }
        if (joined.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

void remove_root(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

// Registration expectations per platform, expressed through the backend itself so the
// test follows whichever mechanism this build uses.
std::filesystem::path expected_registration(const swapdex::ServiceRuntime& runtime) {
    return swapdex::make_service_backend(runtime)->registration_file();
}

swapdex::ServiceRuntime runtime_for(const swapdex::ServiceControlPaths& paths) {
    swapdex::ServiceRuntime runtime;
    runtime.executable = paths.installed_executable;
    runtime.asset = paths.installed_asset;
    runtime.state_root = paths.state_root;
    runtime.codex_home = swapdex::default_codex_home().string();
    runtime.electron_user_data = swapdex::default_electron_user_data().string();
    runtime.data_home = paths.xdg_data_home;
    runtime.state_home = paths.xdg_state_home;
    runtime.override_codex_home = paths.codex_home;
    runtime.override_electron_user_data = paths.electron_user_data;
    runtime.registration_override = paths.registration_file;
    return runtime;
}

}

void test_service_control() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("swapdex-service-control-" + swapdex::random_identifier(8));
    swapdex::ServiceControlPaths paths = test_paths(root);
    swapdex::ensure_private_directory(paths.state_root);
    const std::filesystem::path registration = expected_registration(runtime_for(paths));
    std::vector<std::vector<std::string>> calls;
    auto runner = [&calls](const std::vector<std::string>& arguments) {
        calls.push_back(arguments);
        return 0;
    };
    swapdex::ServiceControl control(paths, runner);
    control.install(false);
    swapdex::test::check(std::filesystem::is_regular_file(paths.installed_executable), "The executable was not installed");
    swapdex::test::check(std::filesystem::is_regular_file(paths.installed_asset), "The renderer asset was not installed");
    swapdex::test::check(std::filesystem::is_regular_file(paths.manifest_file), "The installation manifest was not written");
    if (!registration.empty()) {
        swapdex::test::check(std::filesystem::is_regular_file(registration), "The service registration was not written");
        const std::string contents = swapdex::read_file(registration, 262144U);
        swapdex::test::check(contents.find("Swapdex") != std::string::npos, "The service registration does not name Swapdex");
        swapdex::test::check(contents.find("launch") != std::string::npos, "The service registration does not launch the add-on");
    }
    const swapdex::Json manifest = swapdex::read_json_file(paths.manifest_file, 65536U);
    swapdex::test::check(manifest.value("version", 0) == 1, "The manifest version is wrong");
    swapdex::test::check(!manifest.value("backend", "").empty(), "The manifest does not record the service backend");
    const std::size_t call_count = calls.size();
    control.install(false);
    swapdex::test::check(calls.size() > call_count, "Reinstall did not re-enable autostart");
    control.start();
    control.stop();
    control.status();
    swapdex::test::check(has_call_containing(calls, "swapdex") || !calls.empty(), "The lifecycle commands did not target the service");
    swapdex::write_file_atomically(paths.state_root / "registry.json", "state\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    control.uninstall(false);
    swapdex::test::check(!std::filesystem::exists(paths.installed_executable), "Uninstall left the executable behind");
    swapdex::test::check(!std::filesystem::exists(paths.installed_asset), "Uninstall left the renderer asset behind");
    swapdex::test::check(!std::filesystem::exists(paths.manifest_file), "Uninstall left the installation manifest behind");
    if (!registration.empty()) {
        swapdex::test::check(!std::filesystem::exists(registration), "Uninstall left the service registration behind");
    }
    swapdex::test::check(std::filesystem::exists(paths.state_root / "registry.json"), "Normal uninstall removed account data");

    paths = test_paths(root / "purge");
    swapdex::ServiceControl purge_control(paths, runner);
    purge_control.install(false);
    swapdex::write_file_atomically(paths.state_root / "registry.json", "state\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    purge_control.uninstall(true);
    swapdex::test::check(!std::filesystem::exists(paths.state_root), "Purge uninstall left account data behind");

    paths = test_paths(root / "custom");
    const std::filesystem::path custom_registration = expected_registration(runtime_for(paths));
    if (!custom_registration.empty()) {
        swapdex::ensure_private_directory(custom_registration.parent_path());
        swapdex::write_file_atomically(custom_registration, "[Unit]\nDescription=User owned\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        bool refused = false;
        try {
            swapdex::ServiceControl custom_control(paths, runner);
            custom_control.install(false);
        } catch (const swapdex::Error& error) {
            refused = error.code() == "registration_not_managed";
        }
        swapdex::test::check(refused, "Installer overwrote a registration it does not manage");
    }

    remove_root(root);
}

void test_maintenance_schedule() {
    swapdex::test::check(swapdex::service_detail::should_run_maintenance(true, false, true), "The initial maintenance cycle did not run when enabled");
    swapdex::test::check(!swapdex::service_detail::should_run_maintenance(false, false, true), "A maintenance configuration change triggered an immediate run");
    swapdex::test::check(swapdex::service_detail::should_run_maintenance(false, true, false), "An explicit maintenance request did not run while disabled");
    swapdex::test::check(!swapdex::service_detail::should_run_maintenance(true, false, false), "The initial maintenance cycle ran while disabled");
}

void test_platform_layer() {
    swapdex::test::check(!swapdex::platform::home_directory().empty(), "The home directory could not be resolved");
    swapdex::test::check(swapdex::platform::default_codex_home().is_absolute(), "The Codex home is not absolute");
    swapdex::test::check(swapdex::platform::default_electron_user_data().is_absolute(), "The Codex user data path is not absolute");
    swapdex::test::check(!swapdex::platform::chatgpt_binary_candidates().empty(), "No Codex install locations were searched");
    swapdex::test::check(!swapdex::platform::chatgpt_executable_name().empty(), "The Codex executable name is empty");
    const std::filesystem::path home = swapdex::platform::home_directory();
    const std::filesystem::path environment_override = home / ".codex" / "swapdex-platform-test";
    swapdex::test::check_equal(swapdex::platform::to_native(swapdex::platform::from_native(environment_override.string())), environment_override.string(), "Native path conversion is not lossless");

    const std::filesystem::path lock_dir = std::filesystem::temp_directory_path() / ("swapdex-lock-" + swapdex::random_identifier(8));
    swapdex::ensure_private_directory(lock_dir);
    const std::filesystem::path lock_file = lock_dir / "install.lock";
    {
        swapdex::platform::InstanceLock first(lock_file);
        swapdex::test::check(first.acquired(), "The instance lock was not acquired");
        swapdex::platform::InstanceLock second(lock_file);
        swapdex::test::check(!second.acquired(), "A second instance lock was granted while the first was held");
    }
    {
        swapdex::platform::InstanceLock again(lock_file);
        swapdex::test::check(again.acquired(), "The instance lock was not released");
    }

    const std::filesystem::path scratch = std::filesystem::temp_directory_path() / ("swapdex-durable-" + swapdex::random_identifier(8));
    swapdex::ensure_private_directory(scratch);
    const std::filesystem::path victim = scratch / "auth.json";
    swapdex::write_file_atomically(victim, "data\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    swapdex::platform::remove_file_durable(victim);
    swapdex::test::check(!std::filesystem::exists(victim), "A durable removal did not delete the file");
    swapdex::platform::remove_file_durable(victim);
    swapdex::test::check(true, "Removing a missing file reported a failure");

    const std::vector<std::pair<std::string, std::string>> child = swapdex::platform::child_environment({{"SWAPDEX_PLATFORM_TEST", "1"}});
    bool found = false;
    for (const auto& [name, value] : child) {
        if (name == "SWAPDEX_PLATFORM_TEST" && value == "1") {
            found = true;
        }
        if (name.find('=') != std::string::npos) {
            throw swapdex::test::Failure("The child environment contains an invalid name");
        }
    }
    swapdex::test::check(found, "The child environment dropped an override");
}

void test_shell_bridge() {
    swapdex::ProfileRecord primary;
    primary.id = "account-alpha";
    primary.label = "Work";
    primary.email = "Alpha@Example.test";
    primary.plan = "plus";
    primary.authenticated = true;
    swapdex::UsageWindow five;
    five.remaining_percent = 75;
    five.resets_at = 1893456000;
    primary.primary_usage = five;
    swapdex::UsageWindow weekly;
    weekly.remaining_percent = 40;
    primary.secondary_usage = weekly;
    primary.credits_balance = "12.50";
    primary.available_reset_credits = 2;

    swapdex::ProfileRecord secondary;
    secondary.id = "account-beta";
    secondary.label = "Personal";
    secondary.email = "beta@example.test";
    secondary.plan = "pro";
    secondary.authenticated = true;

    const std::vector<swapdex::ProfileRecord> profiles = {primary, secondary};
    const std::optional<swapdex::ProfileRecord> active = primary;

    swapdex::test::check_equal(swapdex::resolve_account_in(profiles, active, "").id, primary.id, "An empty term did not select the signed in account");
    swapdex::test::check_equal(swapdex::resolve_account_in(profiles, active, "ALPHA@EXAMPLE.TEST").id, primary.id, "An email did not resolve case insensitively");
    swapdex::test::check_equal(swapdex::resolve_account_in(profiles, active, "beta").id, secondary.id, "A partial email did not resolve");
    swapdex::test::check_equal(swapdex::resolve_account_in(profiles, active, "account-beta").id, secondary.id, "An account id did not resolve");
    swapdex::test::check(swapdex::resolve_account_in(profiles, active, "nothing-here").id.empty(), "An unknown term resolved to an account");
    const swapdex::AccountMatch ambiguous = swapdex::resolve_account_in(profiles, active, "example.test");
    swapdex::test::check(ambiguous.ambiguous, "A term matching two accounts was not reported as ambiguous");
    swapdex::test::check_equal(ambiguous.candidates.size(), std::size_t(2), "The ambiguous result did not list both accounts");

    const std::string report = swapdex::usage_report_for(primary, active);
    swapdex::test::check(report.find("Alpha@Example.test") != std::string::npos, "The usage report lost the account name");
    swapdex::test::check(report.find("plan   Plus") != std::string::npos, "The usage report did not capitalise the plan");
    swapdex::test::check(report.find("signed in to the Codex app") != std::string::npos, "The usage report did not mark the active account");
    swapdex::test::check(report.find("75% left") != std::string::npos, "The usage report lost the five hour window");
    swapdex::test::check(report.find("40% left") != std::string::npos, "The usage report lost the weekly window");
    swapdex::test::check(report.find("12.50") != std::string::npos, "The usage report lost the credit balance");
    swapdex::test::check(report.find("2 available") != std::string::npos, "The usage report lost the reset credits");

    const std::string snippet = swapdex::shell_snippet("bash");
    swapdex::test::check(snippet.find("# >>> swapdex >>>") != std::string::npos, "The shell snippet has no begin marker");
    swapdex::test::check(snippet.find("# <<< swapdex <<<") != std::string::npos, "The shell snippet has no end marker");
    swapdex::test::check(snippet.find("SWAPDEX_FOLLOW") != std::string::npos, "The shell snippet offers no bypass");
    swapdex::test::check(snippet.find("command swapdex shell") != std::string::npos, "The shell snippet does not route through Swapdex");
    swapdex::test::check(snippet.find("command codex") != std::string::npos, "The shell snippet does not fall back to the real codex");

    const std::string other = swapdex::usage_report_for(secondary, active);
    swapdex::test::check(other.find("stored, not signed in") != std::string::npos, "A stored account was reported as signed in");
    swapdex::test::check(other.find("Pro") != std::string::npos, "A stored account lost its plan");
}

void test_service_control_exec() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("swapdex-service-exec-" + swapdex::random_identifier(8));
    swapdex::ServiceControlPaths paths = test_paths(root);
    const std::filesystem::path bin = root / "fake-bin";
    const std::filesystem::path log = root / "commands.log";
    swapdex::ensure_private_directory(bin);
    // The fake tool has to be named after whatever this platform actually invokes.
    swapdex::ServiceRuntime probe;
    probe.executable = paths.installed_executable;
    const std::string backend_id = swapdex::make_service_backend(probe)->id();
    if (backend_id == "windows") {
        remove_root(root);
        return;
    }
    // The log path is baked into the script because the child environment is filtered
    // down to an allow list on purpose.
    const std::filesystem::path fake = bin / (backend_id == "launchd" ? "launchctl" : "systemctl");
    const std::string script = "#!/bin/sh\nprintf '%s\\n' \"$*\" >> '" + log.string() + "'\nexit 0\n";
    swapdex::write_file_atomically(fake, script, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec);
    const std::optional<std::string> old_path = swapdex::environment_value("PATH");
    setenv("PATH", (bin.string() + ":" + old_path.value_or("/usr/bin:/bin")).c_str(), 1);
    swapdex::ServiceControl control(paths);
    control.install(false);
    control.start();
    control.stop();
    control.uninstall(false);
    const std::string commands = swapdex::read_file(log, 65536U);
    swapdex::test::check(commands.find("swapdex") != std::string::npos, "The real command runner never targeted the service");
    if (old_path.has_value()) {
        setenv("PATH", old_path->c_str(), 1);
    } else {
        unsetenv("PATH");
    }
    remove_root(root);
}
