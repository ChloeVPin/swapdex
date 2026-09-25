#include "service_control.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>

#include "platform.hpp"
#include "util.hpp"

namespace swapdex {
namespace {

constexpr std::size_t registration_maximum_bytes = 256U * 1024U;
constexpr std::size_t manifest_maximum_bytes = 64U * 1024U;
constexpr std::size_t launcher_maximum_bytes = 64U * 1024U;

std::filesystem::path locate_source_asset() {
    const std::filesystem::path executable(executable_directory());
    const std::vector<std::filesystem::path> candidates = {
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
    paths.source_executable = self.empty() ? std::filesystem::current_path() / "swapdex" : std::filesystem::path(self) / "swapdex";
    if (!regular_file_without_symlink(paths.source_executable)) {
        paths.source_executable = platform::executable_directory() / platform::chatgpt_executable_name();
    }
    paths.source_asset = locate_source_asset();
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

ServiceControl::ServiceControl(ServiceControlPaths paths, ServiceBackend::CommandRunner runner)
    : paths_(std::move(paths)), runner_(std::move(runner)) {}

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
    if (start) {
        std::cout << "Swapdex will start with Codex.\n";
    }
    return 0;
}

int ServiceControl::start() {
    const std::unique_ptr<ServiceBackend> backend = make_service_backend(runtime(), runner_);
    if (!backend->installed()) {
        throw Error("service_not_installed", "Swapdex is not installed. Run the install command first.");
    }
    backend->start();
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

int ServiceControl::uninstall(bool purge_data) {
    ensure_private_directory(paths_.lock_file.parent_path());
    const platform::InstanceLock lock(paths_.lock_file);
    if (!lock.acquired()) {
        throw Error("install_lock_failed", "Another Swapdex installation is already running");
    }
    const std::unique_ptr<ServiceBackend> backend = make_service_backend(runtime(), runner_);
    if (backend->installed()) {
        // Always finish the file cleanup, even if the service manager is unreachable.
        try {
            backend->disable();
        } catch (const std::exception&) {
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
