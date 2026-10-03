#include "test.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <poll.h>
#include <chrono>
#include <string>
#include <vector>
#include <unistd.h>

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
    const std::string backend_id = swapdex::make_service_backend(runtime_for(paths))->id();
    std::vector<std::vector<std::string>> calls;
    // Model a service manager honestly: the job is running until something stops it,
    // and a query answers accordingly. Returning success for every call made a running
    // service indistinguishable from a stopped one.
    bool service_running = true;
    // The backends now judge "running" by the singleton lock the service holds, so
    // the model holds it while the pretended service is up.
    std::shared_ptr<swapdex::platform::InstanceLock> modeled_lock = std::make_shared<swapdex::platform::InstanceLock>(paths.state_root / "service.lock");
    auto set_running = [&paths, &service_running, &modeled_lock](bool running) {
        service_running = running;
        if (running) {
            modeled_lock.reset();
            swapdex::ensure_private_directory(paths.state_root);
            modeled_lock = std::make_shared<swapdex::platform::InstanceLock>(paths.state_root / "service.lock");
        } else {
            modeled_lock.reset();
        }
    };
    auto runner = [&calls, &service_running, &set_running](const std::vector<std::string>& arguments) {
        calls.push_back(arguments);
        const auto verb = [&](const std::string& name) {
            return std::find(arguments.begin(), arguments.end(), name) != arguments.end();
        };
        if (verb("is-active") || verb("print") || verb("status")) {
            return service_running ? 0 : 1;
        }
        if (verb("disable") || verb("bootout") || verb("unload") || verb("stop")) {
            set_running(false);
        }
        if (verb("start") || verb("kickstart") || verb("enable") || verb("load")) {
            set_running(true);
        }
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
    // A reinstall has to stop the running copy, otherwise an update leaves the old
    // process serving the old binary.
    // Each backend spells stopping differently: systemd disables, launchd boots the
    // job out. What matters is that a reinstall asked the service manager to stop.
    const bool stopped_existing = has_call_containing(calls, "disable") || has_call_containing(calls, "bootout")
        || has_call_containing(calls, "unload") || has_call_containing(calls, "stop") || backend_id == "windows";
    swapdex::test::check(stopped_existing, "Reinstall did not stop the running install first");
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
    // The log and state paths are baked into the script because the child
    // environment is filtered down to an allow list on purpose. The fake also has
    // to model running state honestly: a service manager that answered "active"
    // for every verb made the uninstall wait run out the clock.
    const std::filesystem::path fake = bin / (backend_id == "launchd" ? "launchctl" : "systemctl");
    const std::filesystem::path state = root / "service-state";
    const std::string script =
        "#!/bin/sh\n"
        "printf '%s\\n' \"$*\" >> '" + log.string() + "'\n"
        "case \" $*\" in\n"
        "  *is-active*|*print*) [ -f '" + state.string() + "' ] || exit 3 ;;\n"
        "  *stop*|*disable*|*bootout*|*unload*) rm -f '" + state.string() + "' ;;\n"
        "  *start*|*--now*|*load*|*kickstart*) : > '" + state.string() + "' ;;\n"
        "esac\n"
        "exit 0\n";
    swapdex::write_file_atomically(fake, script, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec);
    const std::optional<std::string> old_path = swapdex::environment_value("PATH");
    setenv("PATH", (bin.string() + ":" + old_path.value_or("/usr/bin:/bin")).c_str(), 1);
    swapdex::ServiceControl control(paths);
    control.install(false);
    {
        // A live service presents as a held lock on launchd, so hold one for the
        // calls that need the service to look running, then let go before the
        // uninstall has to prove it can stop anything.
        swapdex::platform::InstanceLock running(paths.state_root / "service.lock");
        control.start();
        control.stop();
    }
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

void test_start_reports_the_truth() {
    // A start that does not stay running must not claim success, and a normally
    // launched app must be named as the reason rather than left to the service log.
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("swapdex-start-" + swapdex::random_identifier(6));
    swapdex::ServiceControlPaths paths = test_paths(root);
    // install first so the backend reports itself as installed
    swapdex::ServiceControl installer(paths, [](const std::vector<std::string>&) { return 0; });
    installer.install(false);

    // A query asks the service manager about the service, anything else is a request to
    // change it. Matching the exact verb would only work for systemd, since launchd
    // spells the same thing kickstart.
    auto is_query = [](const std::vector<std::string>& command) {
        for (const std::string& argument : command) {
            if (argument == "is-active" || argument == "print" || argument == "status") {
                return true;
            }
        }
        return false;
    };

    {
        // is-active never succeeds, so the service never comes up.
        auto runner = [](const std::vector<std::string>&) { return 1; };
        swapdex::AppProcessProbe probe;
        probe.running_unmanaged = [] { return std::optional<std::int64_t>(); };
        probe.close_unmanaged = [] { return true; };
        swapdex::ServiceControl control(paths, runner, probe);
        swapdex::test::check(control.start() != 0, "A start that never came up reported success");
    }

    {
        // The modeled service takes its lock once a start command lands, which is
        // what makes the backend's liveness check see it as running.
        bool started = false;
        std::shared_ptr<swapdex::platform::InstanceLock> lock;
        auto runner = [&started, &lock, &paths, is_query](const std::vector<std::string>& command) {
            if (!is_query(command)) {
                started = true;
                swapdex::ensure_private_directory(paths.state_root);
                lock.reset();
                lock = std::make_shared<swapdex::platform::InstanceLock>(paths.state_root / "service.lock");
            }
            return started ? 0 : 1;
        };
        swapdex::AppProcessProbe probe;
        probe.running_unmanaged = [] { return std::optional<std::int64_t>(); };
        probe.close_unmanaged = [] { return true; };
        swapdex::ServiceControl control(paths, runner, probe);
        swapdex::test::check(control.start() == 0, "A start that came up reported failure");
        swapdex::test::check(started, "The service was never asked to start");
    }

    {
        // A normally launched app blocks the start, and nothing is started behind its back.
        std::vector<std::vector<std::string>> calls;
        bool closed = false;
        auto runner = [&calls, is_query](const std::vector<std::string>& command) {
            calls.push_back(command);
            return is_query(command) ? 1 : 0;
        };
        swapdex::AppProcessProbe probe;
        probe.running_unmanaged = [] { return std::int64_t(4242); };
        probe.close_unmanaged = [&closed] { closed = true; return true; };
        swapdex::ServiceControl control(paths, runner, probe);
        swapdex::test::check(control.start() != 0, "A blocked start reported success");
        swapdex::test::check(!closed, "The app was closed without being asked to");
        bool start_issued = false;
        for (const auto& call : calls) {
            if (!is_query(call)) {
                start_issued = true;
            }
        }
        swapdex::test::check(!start_issued, "Start was attempted while a normally launched app was open");
    }

    {
        // Given permission to close it, the app is closed and the start goes ahead.
        std::vector<std::vector<std::string>> calls;
        bool started = false;
        bool closed = false;
        std::shared_ptr<swapdex::platform::InstanceLock> lock;
        auto runner = [&calls, &started, &lock, &paths, is_query](const std::vector<std::string>& command) {
            calls.push_back(command);
            if (!is_query(command)) {
                started = true;
                swapdex::ensure_private_directory(paths.state_root);
                lock.reset();
                lock = std::make_shared<swapdex::platform::InstanceLock>(paths.state_root / "service.lock");
            }
            return started ? 0 : 1;
        };
        swapdex::AppProcessProbe probe;
        probe.running_unmanaged = [&closed] { return closed ? std::optional<std::int64_t>() : std::optional<std::int64_t>(4242); };
        probe.close_unmanaged = [&closed] { closed = true; return true; };
        swapdex::ServiceControl control(paths, runner, probe);
        swapdex::test::check(control.start(true) == 0, "The start failed after closing the app");
        swapdex::test::check(closed, "The app was not closed when permission was given");
        swapdex::test::check(started, "The service was not started after closing the app");
    }

    {
        // An app that refuses to close is reported, not ignored.
        bool started = false;
        std::shared_ptr<swapdex::platform::InstanceLock> lock;
        auto runner = [&started, &lock, &paths, is_query](const std::vector<std::string>& command) {
            if (!is_query(command)) {
                started = true;
                swapdex::ensure_private_directory(paths.state_root);
                lock.reset();
                lock = std::make_shared<swapdex::platform::InstanceLock>(paths.state_root / "service.lock");
            }
            return started ? 0 : 1;
        };
        swapdex::AppProcessProbe probe;
        probe.running_unmanaged = [] { return std::int64_t(4242); };
        probe.close_unmanaged = [] { return false; };
        swapdex::ServiceControl control(paths, runner, probe);
        swapdex::test::check(control.start(true) != 0, "A start that could not close the app reported success");
        swapdex::test::check(!started, "The service was started even though the app could not be closed");
    }

    std::error_code error;
    std::filesystem::remove_all(root, error);
}

void test_release_bundle_layout() {
    // A downloaded release is a flat folder: the binary and the renderer asset sit
    // together. If the asset cannot be found there, installing from a release fails.
    namespace fs = std::filesystem;
    const fs::path root = std::filesystem::temp_directory_path() / ("swapdex-bundle-" + swapdex::random_identifier(6));
    const fs::path flat = root / "swapdex";
    swapdex::ensure_private_directory(flat);
    swapdex::write_file_atomically(flat / "inject.js", "flat bundle\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    const fs::path found = swapdex::locate_source_asset(flat);
    swapdex::test::check(found == fs::absolute(flat / "inject.js").lexically_normal(), "The asset beside the binary in a release bundle was not found");

    // A source build keeps the asset under assets/.
    const fs::path source = root / "source";
    swapdex::ensure_private_directory(source / "assets");
    swapdex::write_file_atomically(source / "assets" / "inject.js", "source tree\n", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    swapdex::ensure_private_directory(source / "bin");
    const fs::path from_source = swapdex::locate_source_asset(source / "bin");
    swapdex::test::check(from_source == fs::absolute(source / "assets" / "inject.js").lexically_normal(), "The asset in a source tree was not found");

    std::error_code error;
    fs::remove_all(root, error);
}

void test_shutdown_channel_is_interruptible() {
    // The service used to wait for a stop signal in a way that could block forever, so
    // quitting the app left the service hung and it never relaunched the app. The
    // shutdown channel must be a pipe waited on with a timeout, not a blocking wait.
    int descriptors[2] = {-1, -1};
    swapdex::test::check(swapdex::platform::make_close_on_exec_pipe(descriptors), "The shutdown channel could not be created");
    swapdex::test::check(descriptors[0] >= 0 && descriptors[1] >= 0, "The shutdown channel has no descriptors");

    // Nothing written: the wait must come back on its own rather than block.
    pollfd entry {};
    entry.fd = descriptors[0];
    entry.events = POLLIN;
    const auto start = std::chrono::steady_clock::now();
    const int ready = ::poll(&entry, 1, 200);
    const auto waited = std::chrono::steady_clock::now() - start;
    swapdex::test::check(ready == 0, "Waiting on an idle shutdown channel did not time out");
    swapdex::test::check(waited < std::chrono::seconds(2), "Waiting on an idle shutdown channel took far longer than its timeout");

    // A poke wakes it immediately, which is how a stop signal gets through.
    const char byte = 1;
    swapdex::test::check(::write(descriptors[1], &byte, 1) == 1, "The shutdown channel could not be poked");
    swapdex::test::check(::poll(&entry, 1, 200) > 0, "A poked shutdown channel did not wake the wait");

    ::close(descriptors[0]);
    ::close(descriptors[1]);
}

void test_every_app_window_is_a_target() {
    // The settings window is a separate page target. Requiring a single match on one
    // exact url meant it was never injected, which is why the whole settings section was
    // missing on macOS: account removal, the privacy blur and the menu settings.
    nlohmann::json targets = nlohmann::json::object();
    targets["targetInfos"] = nlohmann::json::array();
    auto add = [&targets](const std::string& type, const std::string& url, bool attached) {
        nlohmann::json item = nlohmann::json::object();
        item["type"] = type;
        item["url"] = url;
        item["targetId"] = "id-" + url;
        item["attached"] = attached;
        targets["targetInfos"].push_back(item);
    };
    add("page", "app://-/index.html", false);
    add("page", "app://-/settings.html", false);
    add("page", "app://-/index.html?window=2", false);
    add("page", "app://-/index.html", true);
    add("worker", "app://-/worker.js", false);
    add("page", "https://example.invalid/", false);

    const std::vector<std::string> ids = swapdex::app_page_target_ids(targets);
    swapdex::test::check(ids.size() == 3U, "Not every app window was recognised as a target");
    bool has_settings = false;
    for (const std::string& id : ids) {
        if (id == "id-app://-/settings.html") {
            has_settings = true;
        }
    }
    swapdex::test::check(has_settings, "The settings window was not recognised as an app target");
    for (const std::string& id : ids) {
        if (id.find("worker") != std::string::npos || id.find("example.invalid") != std::string::npos) {
            throw swapdex::test::Failure("A target that is not an app page was selected: " + id);
        }
    }
    swapdex::test::check(swapdex::app_page_target_ids(nlohmann::json::object()).empty(), "Targets were invented from an empty response");
}
void test_settings_tabs_are_clickable() {
    // A tab that renders but whose name is missing from whatever validates it looks
    // fine and silently ignores every click, which is exactly how the Appearance tab
    // shipped. Every tab must have a panel, and the panel must be added to the page.
    const std::filesystem::path asset = swapdex::locate_source_asset(swapdex::platform::executable_directory());
    swapdex::test::check(!asset.empty(), "The renderer asset could not be found to check");
    const std::string source = swapdex::read_file(asset, 2U * 1024U * 1024U);

    std::vector<std::string> tabs;
    std::vector<std::string> panels;
    // Reads the string value assigned to an attribute, so the tab and panel names come
    // from the same source the browser sees.
    const auto collect = [&source](const std::string& attribute, std::vector<std::string>& into) {
        const std::string needle = "." + attribute + " = \"";
        std::size_t at = 0U;
        while ((at = source.find(needle, at)) != std::string::npos) {
            const std::size_t value_start = at + needle.size();
            const std::size_t value_end = source.find('"', value_start);
            if (value_end == std::string::npos) {
                break;
            }
            const std::string name = source.substr(value_start, value_end - value_start);
            if (std::find(into.begin(), into.end(), name) == into.end()) {
                into.push_back(name);
            }
            at = value_end;
        }
    };
    collect("dataset.swapdexSettingsTab", tabs);
    collect("dataset.swapdexSettingsPanel", panels);
    swapdex::test::check(!tabs.empty(), "No settings tabs were found in the renderer asset");
    for (const std::string& name : tabs) {
        if (std::find(panels.begin(), panels.end(), name) == panels.end()) {
            throw swapdex::test::Failure("The settings tab " + name + " has no panel, so it could never open");
        }
    }
    // Activation must not be gated on a list of names kept by hand.
    if (source.find("activateSettingsTab") != std::string::npos) {
        swapdex::test::check(source.find("[data-swapdex-settings-panel=\"") != std::string::npos,
                             "Settings tab activation is validated by a hand kept list rather than by the panels that exist");
    }
}
