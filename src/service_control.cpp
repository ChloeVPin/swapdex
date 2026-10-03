#include "service_control.hpp"

#include <algorithm>
#include <chrono>
#include <thread>
#include <filesystem>
#include <iostream>

#include "platform.hpp"
#include "util.hpp"

namespace swapdex {

std::filesystem::path locate_source_asset(const std::filesystem::path& executable) {
    const std::vector<std::filesystem::path> candidates = {
        // A downloaded bundle is a flat folder, so the asset sits beside the binary.
        executable / "inject.js",
        executable / ".." / "assets" / "inject.js",
        executable / ".." / "share" / "swapdex" / "inject.js",
        platform::state_directory() / "swapdex" / "inject.js",
        std::filesystem::path(SWAPDEX_INJECT_SCRIPT_PATH),
    };
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error && !std::filesystem::is_symlink(candidate)) {
            return std::filesystem::absolute(candidate).lexically_normal();
        }
    }
    return {};
}

namespace {

constexpr std::size_t registration_maximum_bytes = 256U * 1024U;
constexpr std::size_t manifest_maximum_bytes = 64U * 1024U;


bool regular_file_without_symlink(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error && !std::filesystem::is_symlink(path);
}

bool path_entry_exists(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        throw Error("path_inspection_failed", "Unable to inspect an installation path: " + path.string());
    }
    return !error && status.type() != std::filesystem::file_type::not_found;
}

bool is_managed_registration(const std::string& contents) {
    return contents.find("Managed by Swapdex") != std::string::npos || contents.find("swapdex") != std::string::npos;
}

}

ServiceControlPaths default_service_control_paths() {
    const std::filesystem::path home = home_directory();
    const std::optional<std::string> data_home = environment_value("XDG_DATA_HOME");
    const std::optional<std::string> config_home = environment_value("XDG_CONFIG_HOME");
    const std::optional<std::string> state_home = environment_value("XDG_STATE_HOME");
    ServiceControlPaths paths;
    const std::string self = platform::to_native(platform::executable_directory());
    const std::string executable_name =
#if defined(_WIN32)
        "swapdex.exe";
#else
        "swapdex";
#endif
    paths.source_executable = self.empty() ? std::filesystem::current_path() / executable_name : std::filesystem::path(self) / executable_name;
    if (!regular_file_without_symlink(paths.source_executable)) {
        paths.source_executable = platform::executable_directory() / platform::chatgpt_executable_name();
    }
    paths.source_asset = locate_source_asset(platform::executable_directory());
#if defined(_WIN32)
    paths.installed_executable = config_directory() / "swapdex" / "swapdex.exe";
    paths.installed_asset = platform::state_directory() / "swapdex" / "inject.js";
#elif defined(__APPLE__)
    paths.installed_executable = home / ".local" / "bin" / "swapdex";
    paths.installed_asset = home / "Library" / "Application Support" / "swapdex" / "inject.js";
#else
    const std::filesystem::path resolved_data_home = data_home.has_value() ? std::filesystem::path(*data_home) : (home / ".local" / "share");
    paths.installed_executable = home / ".local" / "bin" / "swapdex";
    paths.installed_asset = resolved_data_home / "swapdex" / "inject.js";
#endif
    paths.state_root = state_directory() / "swapdex";
    paths.lock_file = cache_directory() / "swapdex" / "install.lock";
    paths.manifest_file = config_directory() / "swapdex" / "install.json";
    paths.xdg_data_home = data_home;
    paths.xdg_config_home = config_home;
    paths.xdg_state_home = state_home;
    paths.codex_home = environment_value("CODEX_HOME");
    paths.electron_user_data = environment_value("CODEX_ELECTRON_USER_DATA_PATH");
    return paths;
}

std::optional<RuntimePaths> installed_runtime_paths() {
    const std::filesystem::path manifest = config_directory() / "swapdex" / "install.json";
    std::error_code error;
    if (!std::filesystem::exists(manifest, error) || error) {
        return std::nullopt;
    }
    const Json value = read_json_file(manifest, manifest_maximum_bytes);
    if (!value.is_object() || value.value("version", 0) != 1 || !value.contains("state_root") || !value.at("state_root").is_string() || !value.contains("codex_home") || !value.at("codex_home").is_string() || !value.contains("electron_user_data") || !value.at("electron_user_data").is_string()) {
        throw Error("install_manifest_invalid", "The Swapdex installation manifest is invalid");
    }
    RuntimePaths paths{value.at("state_root").get<std::string>(), value.at("codex_home").get<std::string>(), value.at("electron_user_data").get<std::string>()};
    if (!paths.state_root.is_absolute() || !paths.codex_home.is_absolute() || !paths.electron_user_data.is_absolute()) {
        throw Error("install_manifest_invalid", "The Swapdex installation manifest contains an unsafe path");
    }
    return paths;
}

ServiceControl::ServiceControl(ServiceControlPaths paths, ServiceBackend::CommandRunner runner, AppProcessProbe app_probe)
    : paths_(std::move(paths)),
      runner_(std::move(runner)),
      app_probe_(std::move(app_probe)) {
    if (!app_probe_.running_unmanaged) {
        app_probe_.running_unmanaged = [] { return platform::running_unmanaged_chatgpt(-1); };
    }
    if (!app_probe_.close_unmanaged) {
        app_probe_.close_unmanaged = [] { return platform::close_unmanaged_chatgpt(); };
    }
}

ServiceRuntime ServiceControl::runtime() const {
    ServiceRuntime runtime;
    runtime.executable = paths_.installed_executable;
    runtime.asset = paths_.installed_asset;
    runtime.state_root = paths_.state_root;
    runtime.codex_home = paths_.codex_home.value_or(default_codex_home().string());
    runtime.electron_user_data = paths_.electron_user_data.value_or(default_electron_user_data().string());
    runtime.data_home = paths_.xdg_data_home;
    runtime.config_home = paths_.xdg_config_home;
    runtime.state_home = paths_.xdg_state_home;
    runtime.override_codex_home = paths_.codex_home;
    runtime.override_electron_user_data = paths_.electron_user_data;
    runtime.registration_override = paths_.registration_file;
    return runtime;
}

void ServiceControl::validate_sources() const {
    if (paths_.source_executable.empty() || paths_.source_asset.empty() || paths_.installed_executable.empty() || paths_.installed_asset.empty() || paths_.manifest_file.empty()) {
        throw Error("install_path_invalid", "The Swapdex installation paths are incomplete");
    }
    if (!regular_file_without_symlink(paths_.source_asset)) {
        throw Error("source_asset_missing", "The Swapdex interface could not be found");
    }
    std::error_code error;
    if (!std::filesystem::exists(paths_.source_executable, error) || error) {
        throw Error("source_executable_missing", "The Swapdex executable could not be found");
    }
}

void ServiceControl::write_registration(const ServiceBackend& backend) const {
    const std::filesystem::path file = backend.registration_file();
    if (file.empty()) {
        return;
    }
    const std::string desired = backend.registration_contents();
    if (path_entry_exists(file)) {
        if (std::filesystem::is_symlink(file)) {
            throw Error("registration_symlink_refused", "The Swapdex service registration is a symbolic link");
        }
        const std::string current = read_file(file, registration_maximum_bytes);
        if (current != desired && !is_managed_registration(current)) {
            throw Error("registration_not_managed", "Refusing to replace a service registration that Swapdex does not manage");
        }
    }
    ensure_private_directory(file.parent_path());
    write_file_atomically(file, desired, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read | std::filesystem::perms::others_read);
}

void ServiceControl::remove_registration(const ServiceBackend& backend) const {
    const std::filesystem::path file = backend.registration_file();
    if (file.empty()) {
        return;
    }
    if (!path_entry_exists(file)) {
        return;
    }
    if (std::filesystem::is_symlink(file)) {
        throw Error("registration_symlink_refused", "The Swapdex service registration is a symbolic link");
    }
    const std::string current = read_file(file, registration_maximum_bytes);
    if (!is_managed_registration(current)) {
        throw Error("registration_not_managed", "Refusing to remove a service registration that Swapdex does not manage");
    }
    std::error_code error;
    if (!std::filesystem::remove(file, error) || error) {
        throw Error("registration_cleanup_failed", "Unable to remove the Swapdex service registration");
    }
}

void ServiceControl::remove_installed_file(const std::filesystem::path& path) const {
    if (!path_entry_exists(path)) {
        return;
    }
    std::error_code error;
    if (std::filesystem::is_symlink(path)) {
        if (!std::filesystem::remove(path, error) || error) {
            throw Error("installed_cleanup_failed", "Unable to remove an installed Swapdex link");
        }
        return;
    }
    if (std::filesystem::is_directory(path, error) && !error) {
        throw Error("installed_path_directory", "Refusing to remove an installation directory in place of a file");
    }
    if (!std::filesystem::remove(path, error) || error) {
        throw Error("installed_cleanup_failed", "Unable to remove an installed Swapdex file");
    }
}

int ServiceControl::install(bool start) {
    ensure_private_directory(paths_.lock_file.parent_path());
    const platform::InstanceLock lock(paths_.lock_file);
    if (!lock.acquired()) {
        throw Error("install_lock_failed", "Another Swapdex installation is already running");
    }
    validate_sources();
    const std::unique_ptr<ServiceBackend> backend = make_service_backend(runtime(), runner_);
    // A previous install is already running, so it has to be stopped before the new
    // binary is started. Without this an update leaves the old process in place.
    const bool replacing = backend->installed();
    const std::filesystem::path executable_parent = paths_.installed_executable.parent_path();
    const std::filesystem::path asset_parent = paths_.installed_asset.parent_path();
    ensure_directory(executable_parent, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec | std::filesystem::perms::group_read | std::filesystem::perms::group_exec | std::filesystem::perms::others_read | std::filesystem::perms::others_exec);
    ensure_private_directory(asset_parent);
    ensure_private_directory(paths_.manifest_file.parent_path());
    // launchd opens the service log before the service runs, so its directory has to
    // exist already rather than being created by the service itself.
    ensure_private_directory(paths_.state_root);
    copy_file_atomically(paths_.source_executable, paths_.installed_executable, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec);
    copy_file_atomically(paths_.source_asset, paths_.installed_asset, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read | std::filesystem::perms::others_read);
    write_registration(*backend);
    const Json manifest = {
        {"version", 1},
        {"backend", backend->id()},
        {"state_root", paths_.state_root.string()},
        {"codex_home", paths_.codex_home.value_or(default_codex_home().string())},
        {"electron_user_data", paths_.electron_user_data.value_or(default_electron_user_data().string())}
    };
    write_json_file_atomically(paths_.manifest_file, manifest, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    if (replacing) {
        try {
            backend->disable();
        } catch (const std::exception&) {
        }
    }
    backend->enable(start);
    std::cout << "Swapdex installed using " << backend->id() << ".\n";
    if (!start) {
        return 0;
    }
    // An install replaces a running copy, so the old job is booted out and the new one
    // loaded. That handover is not atomic, and a load issued too soon after a bootout
    // can leave nothing loaded at all. Confirm it came up, and retry the load once
    // before giving up, rather than reporting an install that is not running.
    for (int attempt = 0; attempt < 100 && !backend->active(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!backend->active()) {
        std::cout << "Retrying the service registration.\n";
        try {
            static_cast<void>(backend->enable(true));
        } catch (const std::exception&) {
        }
        for (int attempt = 0; attempt < 100 && !backend->active(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (!backend->active()) {
        std::cerr << "swapdex: the files are installed but the service did not start.\n";
        std::cerr << "Run swapdex status for details, then swapdex start.\n";
        return 1;
    }
    std::cout << "Swapdex will start with Codex.\n";
    return 0;
}

int ServiceControl::start(bool close_app) {
    const std::unique_ptr<ServiceBackend> backend = make_service_backend(runtime(), runner_);
    if (!backend->installed()) {
        throw Error("service_not_installed", "Swapdex is not installed. Run the install command first.");
    }
    if (backend->active()) {
        std::cout << "Swapdex is already running.\n";
        return 0;
    }
    // A managed instance a dead service left behind looks unmanaged but carries the
    // debug pipe flag, so it is reclaimed quietly. Only a genuinely foreign app makes
    // a start refuse.
    for (int guard = 0; guard < 8; ++guard) {
        const std::optional<std::int64_t> orphan = app_probe_.running_unmanaged();
        if (!orphan.has_value() || !platform::chatgpt_process_is_managed(*orphan)) {
            break;
        }
        platform::close_process(*orphan);
    }
    // Swapdex launches the app itself so it can attach to it. A normally launched app
    // holds the same profile, so it has to be closed first, and saying so is more use
    // than a service that quietly fails to start.
    if (app_probe_.running_unmanaged().has_value()) {
        if (!close_app) {
            std::cerr << "The Codex app is already open. Swapdex needs to close it and start its own.\n";
            std::cerr << "Close the app and run this again, or run swapdex start --close-app.\n";
            return 1;
        }
        std::cout << "Closing the Codex app.\n";
        if (!app_probe_.close_unmanaged()) {
            std::cerr << "swapdex: the Codex app did not close. Quit it yourself and run this again.\n";
            return 1;
        }
    }
    try {
        backend->start();
    } catch (const std::exception&) {
        // A refused start is not the end of the story, and the reason is more useful
        // than the raw backend complaint, so fall through and report the real outcome.
    }
    for (int attempt = 0; attempt < 100 && !backend->active(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!backend->active()) {
        std::cerr << "swapdex: the service did not stay running.\n";
        std::cerr << "Run swapdex status for details.\n";
        return 1;
    }
    std::cout << "Swapdex started.\n";
    return 0;
}

int ServiceControl::stop() {
    const std::unique_ptr<ServiceBackend> backend = make_service_backend(runtime(), runner_);
    if (!backend->installed()) {
        std::cout << "Swapdex is not installed.\n";
        return 0;
    }
    backend->stop();
    std::cout << "Swapdex stopped.\n";
    return 0;
}

int ServiceControl::status() {
    const std::unique_ptr<ServiceBackend> backend = make_service_backend(runtime(), runner_);
    if (!backend->installed()) {
        std::cout << "Swapdex is not installed.\n";
        return 1;
    }
    return backend->status();
}

int ServiceControl::doctor() {
    // A health check a user can paste into a bug report: every line says what was
    // looked at and what to do about it, and the exit code counts real failures.
    std::cout << "swapdex " << SWAPDEX_VERSION << "\n";
    int failures = 0;
    int warnings = 0;
    const auto ok = [](const std::string& text) { std::cout << "ok    " << text << "\n"; };
    const auto warn = [&warnings](const std::string& text) {
        ++warnings;
        std::cout << "warn  " << text << "\n";
    };
    const auto fail = [&failures](const std::string& text) {
        ++failures;
        std::cout << "fail  " << text << "\n";
    };

    const std::unique_ptr<ServiceBackend> backend = make_service_backend(runtime(), runner_);

    if (path_entry_exists(paths_.installed_executable)) {
        ok("binary installed at " + paths_.installed_executable.string());
    } else {
        fail("no installed binary at " + paths_.installed_executable.string() + "; run swapdex install");
    }

    if (path_entry_exists(paths_.installed_asset)) {
        // A stale interface asset leaves the service injecting an old build, so
        // drift from the asset beside this binary is a warning rather than trivia.
        if (path_entry_exists(paths_.source_asset) && read_file(paths_.installed_asset, 4U << 20) != read_file(paths_.source_asset, 4U << 20)) {
            warn("the installed interface differs from this build; rerun the installer");
        } else {
            ok("interface asset installed");
        }
    } else {
        fail("no installed interface at " + paths_.installed_asset.string() + "; run swapdex install");
    }

    if (backend->installed()) {
        ok("service registered with " + backend->id());
    } else {
        fail("the service is not registered; run swapdex install");
    }

    if (backend->active()) {
        ok("service running");
    } else if (backend->installed()) {
        warn("the service is installed but not running; run swapdex start");
    }

    if (path_entry_exists(paths_.state_root)) {
        const std::filesystem::perms exposed = std::filesystem::status(paths_.state_root).permissions() & (std::filesystem::perms::group_all | std::filesystem::perms::others_all);
        if (exposed == std::filesystem::perms::none) {
            ok("account data directory is private");
        } else {
            warn("account data directory " + paths_.state_root.string() + " is readable by other users");
        }
    }

    const std::filesystem::path app = platform::find_chatgpt_binary();
    if (!app.empty()) {
        ok("Codex found at " + app.string());
    } else {
        fail("the Codex app was not found; install it and sign in first");
    }

    if (const auto foreign = app_probe_.running_unmanaged(); foreign.has_value()) {
        if (platform::chatgpt_process_is_managed(*foreign)) {
            ok("a managed Codex is running (pid " + std::to_string(*foreign) + ")");
        } else {
            warn("a Codex opened outside Swapdex is running; the service adopts it on its next check");
        }
    } else {
        ok("no stray Codex process");
    }

    // The service does not care where its binary lives, but the commands do.
    bool on_path = false;
    const std::string wanted = paths_.installed_executable.parent_path().string();
    if (const auto path = environment_value("PATH"); path.has_value()) {
        const std::string directories = *path + ":";
        for (std::size_t position = 0; position < directories.size();) {
            const std::size_t colon = directories.find(':', position);
            if (directories.substr(position, colon - position) == wanted) {
                on_path = true;
                break;
            }
            position = colon + 1;
        }
    }
    if (on_path) {
        ok("swapdex is on PATH");
    } else {
        warn(wanted + " is not on PATH; the swapdex command will not resolve there");
    }

    std::cout << failures << " failure(s), " << warnings << " warning(s)\n";
    return failures == 0 ? 0 : 1;
}

int ServiceControl::uninstall(bool purge_data) {
    ensure_private_directory(paths_.lock_file.parent_path());
    const platform::InstanceLock lock(paths_.lock_file);
    if (!lock.acquired()) {
        throw Error("install_lock_failed", "Another Swapdex installation is already running");
    }
    const std::unique_ptr<ServiceBackend> backend = make_service_backend(runtime(), runner_);
    if (backend->installed()) {
        // Always finish the file cleanup, even if the service manager is unreachable,
        // but never claim success while something is still running. A failed bootout used
        // to be swallowed, which left a live service with its files deleted.
        try {
            backend->disable();
        } catch (const std::exception& error) {
            std::string reason = error.what();
            std::cerr << "swapdex: the service could not be stopped: " << reason << "\n";
        }
        // Whatever the unload call returned, the honest question is whether a service
        // is still alive: bootout marks the job down before the process finishes
        // tearing down, and the teardown itself can take a moment.
        bool stopped = !backend->active();
        for (int attempt = 0; !stopped && attempt < 300; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stopped = !backend->active();
        }
        if (!stopped) {
            std::cerr << "swapdex: the service is still running, so it was not removed.\n";
            std::cerr << "Run swapdex status, stop it, then uninstall again.\n";
            return 1;
        }
        remove_registration(*backend);
    }
    remove_installed_file(paths_.installed_executable);
    remove_installed_file(paths_.installed_asset);
    remove_installed_file(paths_.manifest_file);
    if (purge_data) {
        // Account homes live outside the state root, so both have to go.
        for (const std::filesystem::path& root : {paths_.state_root, default_account_root()}) {
            if (!path_entry_exists(root)) {
                continue;
            }
            std::error_code error;
            if (std::filesystem::is_symlink(root) || !std::filesystem::is_directory(root, error) || error) {
                throw Error("purge_path_invalid", "Refusing to purge a non-directory Swapdex data path");
            }
            std::filesystem::remove_all(root, error);
            if (error) {
                throw Error("purge_failed", "Unable to remove Swapdex account data");
            }
        }
        std::cout << "Swapdex account data removed, including stored sign ins.\n";
    } else {
        std::cout << "Swapdex account data preserved in " << default_account_root().string() << "\n";
    }
    std::cout << "Swapdex uninstalled.\n";
    return 0;
}

}
