#include "test.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "service.hpp"
#include "service_control.hpp"
#include "util.hpp"

namespace {

swapdex::ServiceControlPaths test_paths(std::filesystem::path root) {
    root /= "space root";
    swapdex::ServiceControlPaths paths;
    paths.source_executable = root / "source" / "swapdex";
    paths.source_asset = root / "source" / "inject.js";
    paths.installed_executable = root / "install" / "bin" / "swapdex";
    paths.installed_asset = root / "install" / "share" / "swapdex" / "inject.js";
    paths.unit_file = root / "config" / "systemd" / "user" / "swapdex.service";
    paths.legacy_desktop_file = root / "data" / "applications" / "swapdex-codex.desktop";
    paths.legacy_desktop_file_fallback = root / "fallback-applications" / "swapdex-codex.desktop";
    paths.state_root = root / "state" / "swapdex";
    paths.lock_file = root / "config" / "swapdex" / "install.lock";
    paths.manifest_file = root / ".swapdex" / "install.json";
    paths.xdg_data_home = (root / "data").string();
    paths.xdg_state_home = (root / "state-root").string();
    swapdex::write_file_atomically(paths.source_executable, "binary\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec);
    swapdex::write_file_atomically(paths.source_asset, "asset\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    return paths;
}

bool has_call(const std::vector<std::vector<std::string>>& calls, const std::vector<std::string>& expected) {
    return std::find(calls.begin(), calls.end(), expected) != calls.end();
}

void remove_root(const std::filesystem::path& root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
}

}

void test_service_control() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("swapdex-service-control-" + swapdex::random_identifier(8));
    swapdex::ServiceControlPaths paths = test_paths(root);
    swapdex::ensure_private_directory(paths.state_root);
    std::vector<std::vector<std::string>> calls;
    auto runner = [&calls](const std::vector<std::string>& arguments) {
        calls.push_back(arguments);
        return 0;
    };
    swapdex::write_file_atomically(paths.legacy_desktop_file_fallback, "[Desktop Entry]\nName=Swapdex Codex\nExec=swapdex launch\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    swapdex::ServiceControl control(paths, runner);
    control.install(false);
    swapdex::test::check(std::filesystem::is_regular_file(paths.installed_executable), "The executable was not installed");
    swapdex::test::check(std::filesystem::is_regular_file(paths.installed_asset), "The renderer asset was not installed");
    swapdex::test::check(std::filesystem::is_regular_file(paths.unit_file), "The service unit was not installed");
    swapdex::test::check(std::filesystem::is_regular_file(paths.manifest_file), "The installation manifest was not written");
    swapdex::test::check(!std::filesystem::exists(paths.legacy_desktop_file_fallback), "The legacy application launcher was not removed");
    const std::string unit = swapdex::read_file(paths.unit_file, 65536U);
    swapdex::test::check(unit.find("# Managed by Swapdex") == 0, "The service unit is missing its ownership marker");
    swapdex::test::check(unit.find("ExecStart=\"" + paths.installed_executable.string() + "\"") != std::string::npos, "The service unit does not quote its executable");
    swapdex::test::check(unit.find("Environment=\"XDG_DATA_HOME=") != std::string::npos, "The service unit does not preserve XDG data paths");
    swapdex::test::check(unit.find("Environment=\"XDG_STATE_HOME=") != std::string::npos, "The service unit does not preserve XDG state paths");
    swapdex::test::check(has_call(calls, {"--user", "daemon-reload"}), "The installer did not reload systemd");
    swapdex::test::check(has_call(calls, {"--user", "enable", "swapdex.service"}), "The installer did not enable autostart");
    const auto call_count = calls.size();
    control.install(false);
    swapdex::test::check(calls.size() == call_count + 2U, "Reinstall was not idempotent");
    control.start();
    control.stop();
    control.status();
    swapdex::test::check(has_call(calls, {"--user", "start", "swapdex.service"}), "The start command did not target the service");
    swapdex::test::check(has_call(calls, {"--user", "stop", "swapdex.service"}), "The stop command did not target the service");
    swapdex::write_file_atomically(paths.state_root / "registry.json", "state\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    control.uninstall(false);
    swapdex::test::check(!std::filesystem::exists(paths.installed_executable), "Uninstall left the executable behind");
    swapdex::test::check(!std::filesystem::exists(paths.installed_asset), "Uninstall left the renderer asset behind");
    swapdex::test::check(!std::filesystem::exists(paths.unit_file), "Uninstall left the service unit behind");
    swapdex::test::check(!std::filesystem::exists(paths.manifest_file), "Uninstall left the installation manifest behind");
    swapdex::test::check(std::filesystem::exists(paths.state_root / "registry.json"), "Normal uninstall removed account data");
    swapdex::test::check(has_call(calls, {"--user", "disable", "--now", "swapdex.service"}), "Uninstall did not disable the service");

    paths = test_paths(root / "purge");
    swapdex::ServiceControl purge_control(paths, runner);
    purge_control.install(false);
    swapdex::write_file_atomically(paths.state_root / "registry.json", "state\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    purge_control.uninstall(true);
    swapdex::test::check(!std::filesystem::exists(paths.state_root), "Purge uninstall left account data behind");

    paths = test_paths(root / "no-systemctl");
    swapdex::ServiceControl installed_without_manager(paths, runner);
    installed_without_manager.install(false);
    swapdex::ServiceControl unavailable_manager(paths, [](const std::vector<std::string>&) { return 127; });
    unavailable_manager.uninstall(false);
    swapdex::test::check(!std::filesystem::exists(paths.unit_file), "Uninstall could not clean up without systemd");

    paths = test_paths(root / "custom");
    swapdex::write_file_atomically(paths.unit_file, "[Unit]\nDescription=User unit\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    bool refused = false;
    try {
        swapdex::ServiceControl custom_control(paths, runner);
        custom_control.install(false);
    } catch (const swapdex::Error& error) {
        refused = error.code() == "unit_not_managed";
    }
    swapdex::test::check(refused, "Installer overwrote a user-owned service unit");
    swapdex::test::check(!std::filesystem::exists(paths.installed_executable), "Installer changed files before rejecting a user unit");

    remove_root(root);
}

void test_maintenance_schedule() {
    swapdex::test::check(swapdex::service_detail::should_run_maintenance(true, false, true), "The initial maintenance cycle did not run when enabled");
    swapdex::test::check(!swapdex::service_detail::should_run_maintenance(false, false, true), "A maintenance configuration change triggered an immediate run");
    swapdex::test::check(swapdex::service_detail::should_run_maintenance(false, true, false), "An explicit maintenance request did not run while disabled");
    swapdex::test::check(!swapdex::service_detail::should_run_maintenance(true, false, false), "The initial maintenance cycle ran while disabled");
}

void test_service_control_exec() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("swapdex-service-exec-" + swapdex::random_identifier(8));
    swapdex::ServiceControlPaths paths = test_paths(root);
    const std::filesystem::path bin = root / "fake-bin";
    const std::filesystem::path log = root / "systemctl.log";
    swapdex::ensure_private_directory(bin);
    const std::filesystem::path fake = bin / "systemctl";
    swapdex::write_file_atomically(fake, "#!/bin/sh\nprintf '%s\\n' \"$*\" >> \"$SWAPDEX_TEST_SYSTEMCTL_LOG\"\nexit 0\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec);
    const std::optional<std::string> old_path = swapdex::environment_value("PATH");
    const std::optional<std::string> old_log = swapdex::environment_value("SWAPDEX_TEST_SYSTEMCTL_LOG");
    setenv("PATH", (bin.string() + ":" + old_path.value_or("/usr/bin:/bin")).c_str(), 1);
    setenv("SWAPDEX_TEST_SYSTEMCTL_LOG", log.c_str(), 1);
    swapdex::ServiceControl control(paths);
    control.install(false);
    control.start();
    control.stop();
    control.uninstall(false);
    const std::string commands = swapdex::read_file(log, 65536U);
    swapdex::test::check(commands.find("--user daemon-reload") != std::string::npos, "The real command runner did not reload systemd");
    swapdex::test::check(commands.find("--user enable swapdex.service") != std::string::npos, "The real command runner did not enable the unit");
    swapdex::test::check(commands.find("--user start swapdex.service") != std::string::npos, "The real command runner did not start the unit");
    swapdex::test::check(commands.find("--user stop swapdex.service") != std::string::npos, "The real command runner did not stop the unit");
    if (old_path.has_value()) {
        setenv("PATH", old_path->c_str(), 1);
    } else {
        unsetenv("PATH");
    }
    if (old_log.has_value()) {
        setenv("SWAPDEX_TEST_SYSTEMCTL_LOG", old_log->c_str(), 1);
    } else {
        unsetenv("SWAPDEX_TEST_SYSTEMCTL_LOG");
    }
    remove_root(root);
}
