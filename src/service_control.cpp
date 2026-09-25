#include "service_control.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <system_error>
#include <vector>

#include "util.hpp"

namespace swapdex {
namespace {

constexpr const char* service_name = "swapdex.service";
constexpr std::size_t unit_maximum_bytes = 64U * 1024U;
constexpr std::size_t desktop_maximum_bytes = 64U * 1024U;
constexpr std::size_t manifest_maximum_bytes = 64U * 1024U;

class ManagementLock {
public:
    explicit ManagementLock(const std::filesystem::path& path) {
        const std::filesystem::path parent = path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
        ensure_private_directory(parent);
        descriptor_ = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor_ < 0) {
            throw Error("install_lock_failed", "Unable to open the installation lock");
        }
        if (flock(descriptor_, LOCK_EX) != 0) {
            close(descriptor_);
            descriptor_ = -1;
            throw Error("install_lock_failed", "Unable to lock the installation directory");
        }
    }

    ~ManagementLock() {
        if (descriptor_ >= 0) {
            flock(descriptor_, LOCK_UN);
            close(descriptor_);
        }
    }

    ManagementLock(const ManagementLock&) = delete;
    ManagementLock& operator=(const ManagementLock&) = delete;

private:
    int descriptor_ = -1;
};

std::filesystem::path executable_path() {
    std::error_code error;
    const std::filesystem::path resolved = std::filesystem::canonical("/proc/self/exe", error);
    if (!error && !resolved.empty()) {
        return resolved;
    }
    return std::filesystem::path(executable_directory()) / "swapdex";
}

std::filesystem::path locate_source_asset() {
    const std::filesystem::path executable = std::filesystem::path(executable_directory());
    const std::filesystem::path data_home = std::filesystem::path(environment_value("XDG_DATA_HOME").value_or((home_directory() / ".local" / "share").string()));
    const std::array<std::filesystem::path, 4> candidates = {
        executable / ".." / "assets" / "inject.js",
        executable / ".." / "share" / "swapdex" / "inject.js",
        data_home / "swapdex" / "inject.js",
        std::filesystem::path(SWAPDEX_INJECT_SCRIPT_PATH),
    };
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error && !std::filesystem::is_symlink(candidate)) {
            return std::filesystem::absolute(candidate).lexically_normal();
        }
    }
    return std::filesystem::absolute(candidates.front()).lexically_normal();
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

std::string quote_systemd_value(const std::string& value) {
    std::string quoted = "\"";
    for (const unsigned char character : value) {
        if (character == '\n' || character == '\r' || character == '\0') {
            throw Error("unit_path_invalid", "The installed path cannot be represented in a service unit");
        }
        if (character == '%') {
            quoted.append("%%");
            continue;
        }
        if (character == '\\' || character == '"') {
            quoted.push_back('\\');
        }
        quoted.push_back(static_cast<char>(character));
    }
    quoted.push_back('"');
    return quoted;
}

std::string quote_systemd_path(const std::filesystem::path& path) {
    return quote_systemd_value(path.lexically_normal().string());
}

void append_environment(std::ostringstream& unit, const char* name, const std::optional<std::string>& value) {
    if (value.has_value()) {
        unit << "Environment=" << quote_systemd_value(std::string(name) + "=" + *value) << "\n";
    }
}

std::string unit_contents(const ServiceControlPaths& paths) {
    std::ostringstream unit;
    unit << "# Managed by Swapdex\n"
         << "[Unit]\n"
         << "Description=Swapdex Codex account service\n"
         << "After=graphical-session.target\n"
         << "StartLimitIntervalSec=60\n"
         << "StartLimitBurst=3\n\n"
         << "[Service]\n"
         << "Type=simple\n"
         << "ExecStart=" << quote_systemd_path(paths.installed_executable) << " launch\n"
         << "Restart=on-failure\n"
         << "RestartSec=5\n"
         << "KillMode=control-group\n"
         << "TimeoutStopSec=60\n"
         << "WorkingDirectory=%h\n"
         << "UMask=0077\n";
    append_environment(unit, "XDG_DATA_HOME", paths.xdg_data_home);
    append_environment(unit, "XDG_CONFIG_HOME", paths.xdg_config_home);
    append_environment(unit, "XDG_STATE_HOME", paths.xdg_state_home);
    append_environment(unit, "CODEX_HOME", paths.codex_home);
    append_environment(unit, "CODEX_ELECTRON_USER_DATA_PATH", paths.electron_user_data);
    unit << "\n[Install]\n"
         << "WantedBy=default.target\n";
    return unit.str();
}

int run_systemctl(const std::vector<std::string>& arguments) {
    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1U);
    storage.emplace_back("systemctl");
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1U);
    for (std::string& argument : storage) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) {
        throw Error("systemctl_spawn_failed", "Unable to start systemctl");
    }
    if (child == 0) {
        execvp(argv.front(), argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            throw Error("systemctl_wait_failed", "Unable to wait for systemctl");
        }
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

void require_regular_file(const std::filesystem::path& path, const char* code, const char* message) {
    if (!regular_file_without_symlink(path)) {
        throw Error(code, message);
    }
}

}

ServiceControlPaths default_service_control_paths() {
    const std::filesystem::path home = home_directory();
    const std::optional<std::string> xdg_data_home = environment_value("XDG_DATA_HOME");
    const std::optional<std::string> xdg_config_home = environment_value("XDG_CONFIG_HOME");
    const std::optional<std::string> xdg_state_home = environment_value("XDG_STATE_HOME");
    const std::optional<std::string> codex_home = environment_value("CODEX_HOME");
    const std::optional<std::string> electron_user_data = environment_value("CODEX_ELECTRON_USER_DATA_PATH");
    const std::filesystem::path data_home = xdg_data_home.value_or((home / ".local" / "share").string());
    const std::filesystem::path config_home = xdg_config_home.value_or((home / ".config").string());
    ServiceControlPaths paths;
    paths.source_executable = executable_path();
    paths.source_asset = locate_source_asset();
    paths.installed_executable = home / ".local" / "bin" / "swapdex";
    paths.installed_asset = std::filesystem::path(data_home) / "swapdex" / "inject.js";
    paths.unit_file = std::filesystem::path(config_home) / "systemd" / "user" / service_name;
    paths.legacy_desktop_file = std::filesystem::path(data_home) / "applications" / "swapdex-codex.desktop";
    paths.legacy_desktop_file_fallback = home / ".local" / "share" / "applications" / "swapdex-codex.desktop";
    paths.state_root = state_directory() / "swapdex";
    paths.lock_file = std::filesystem::path(config_home) / "swapdex" / "install.lock";
    paths.manifest_file = home / ".swapdex" / "install.json";
    paths.xdg_data_home = xdg_data_home;
    paths.xdg_config_home = xdg_config_home;
    paths.xdg_state_home = xdg_state_home;
    paths.codex_home = codex_home;
    paths.electron_user_data = electron_user_data;
    return paths;
}

std::optional<RuntimePaths> installed_runtime_paths() {
    const std::filesystem::path manifest = home_directory() / ".swapdex" / "install.json";
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

ServiceControl::ServiceControl(ServiceControlPaths paths, SystemdCommandRunner runner)
    : paths_(std::move(paths)), runner_(std::move(runner)) {}

int ServiceControl::run(const std::vector<std::string>& arguments) const {
    const int result = runner_ ? runner_(arguments) : run_systemctl(arguments);
    if (result != 0) {
        throw Error("systemctl_command_failed", "systemctl could not complete the requested service operation");
    }
    return result;
}

int ServiceControl::run_optional_systemctl(const std::vector<std::string>& arguments) const {
    return runner_ ? runner_(arguments) : run_systemctl(arguments);
}

void ServiceControl::validate_paths() const {
    if (paths_.source_executable.empty() || paths_.source_asset.empty() || paths_.installed_executable.empty() || paths_.installed_asset.empty() || paths_.unit_file.empty() || paths_.manifest_file.empty()) {
        throw Error("install_path_invalid", "The Swapdex installation paths are incomplete");
    }
    require_regular_file(paths_.source_executable, "source_executable_missing", "The Swapdex executable could not be found");
    require_regular_file(paths_.source_asset, "source_asset_missing", "The Swapdex renderer integration could not be found");
}

void ServiceControl::validate_unit() const {
    const std::string desired = unit_contents(paths_);
    if (path_entry_exists(paths_.unit_file)) {
        if (std::filesystem::is_symlink(paths_.unit_file)) {
            throw Error("unit_symlink_refused", "The Swapdex service unit is a symbolic link");
        }
        const std::string current = read_file(paths_.unit_file, unit_maximum_bytes);
        if (current != desired && current.find("# Managed by Swapdex") == std::string::npos) {
            throw Error("unit_not_managed", "Refusing to replace a service unit not managed by Swapdex");
        }
    }
}

void ServiceControl::write_unit() const {
    validate_unit();
    const std::string desired = unit_contents(paths_);
    write_file_atomically(paths_.unit_file, desired, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read | std::filesystem::perms::others_read);
}

void ServiceControl::remove_legacy_desktop() const {
    std::vector<std::filesystem::path> candidates = {paths_.legacy_desktop_file, paths_.legacy_desktop_file_fallback};
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    for (const auto& path : candidates) {
        std::error_code error;
        if (!path_entry_exists(path) || std::filesystem::is_symlink(path) || !std::filesystem::is_regular_file(path, error) || error) {
            continue;
        }
        const std::string contents = read_file(path, desktop_maximum_bytes);
        if (contents.find("Swapdex Codex") != std::string::npos && contents.find("swapdex") != std::string::npos) {
            std::filesystem::remove(path, error);
            if (error) {
                throw Error("desktop_cleanup_failed", "Unable to remove the legacy Swapdex launcher");
            }
        }
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

void ServiceControl::remove_unit() const {
    if (!path_entry_exists(paths_.unit_file)) {
        return;
    }
    if (std::filesystem::is_symlink(paths_.unit_file)) {
        throw Error("unit_symlink_refused", "The Swapdex service unit is a symbolic link");
    }
    std::error_code error;
    if (!std::filesystem::remove(paths_.unit_file, error) || error) {
        throw Error("unit_cleanup_failed", "Unable to remove the Swapdex service unit");
    }
}

int ServiceControl::install(bool start) {
    const ManagementLock lock(paths_.lock_file);
    validate_paths();
    validate_unit();
    const std::filesystem::path executable_parent = paths_.installed_executable.parent_path();
    const std::filesystem::path asset_parent = paths_.installed_asset.parent_path();
    const std::filesystem::path unit_parent = paths_.unit_file.parent_path();
    ensure_directory(executable_parent, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec | std::filesystem::perms::group_read | std::filesystem::perms::group_exec | std::filesystem::perms::others_read | std::filesystem::perms::others_exec);
    ensure_private_directory(asset_parent);
    ensure_private_directory(unit_parent);
    copy_file_atomically(paths_.source_executable, paths_.installed_executable, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec);
    copy_file_atomically(paths_.source_asset, paths_.installed_asset, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::group_read | std::filesystem::perms::others_read);
    write_unit();
    const Json manifest = {
        {"version", 1},
        {"state_root", paths_.state_root.string()},
        {"codex_home", paths_.codex_home.value_or(default_codex_home().string())},
        {"electron_user_data", paths_.electron_user_data.value_or(default_electron_user_data().string())}
    };
    write_json_file_atomically(paths_.manifest_file, manifest, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    remove_legacy_desktop();
    run({"--user", "daemon-reload"});
    if (start) {
        run({"--user", "enable", "--now", service_name});
        std::cout << "Swapdex installed and started.\n";
    } else {
        run({"--user", "enable", service_name});
        std::cout << "Swapdex installed and enabled.\n";
    }
    return 0;
}

int ServiceControl::start() {
    if (!regular_file_without_symlink(paths_.unit_file)) {
        throw Error("service_not_installed", "Swapdex is not installed; run swapdex install first");
    }
    run({"--user", "start", service_name});
    std::cout << "Swapdex started.\n";
    return 0;
}

int ServiceControl::stop() {
    if (!regular_file_without_symlink(paths_.unit_file)) {
        std::cout << "Swapdex is not installed.\n";
        return 0;
    }
    run({"--user", "stop", service_name});
    std::cout << "Swapdex stopped.\n";
    return 0;
}

int ServiceControl::status() {
    if (!regular_file_without_symlink(paths_.unit_file)) {
        std::cout << "Swapdex is not installed.\n";
        return 1;
    }
    return runner_ ? runner_({"--user", "status", "--no-pager", "--full", service_name}) : run_systemctl({"--user", "status", "--no-pager", "--full", service_name});
}

int ServiceControl::uninstall(bool purge_data) {
    const ManagementLock lock(paths_.lock_file);
    std::error_code error;
    const bool unit_exists = path_entry_exists(paths_.unit_file);
    if (unit_exists) {
        if (std::filesystem::is_symlink(paths_.unit_file)) {
            throw Error("unit_symlink_refused", "The Swapdex service unit is a symbolic link");
        }
        const std::string contents = read_file(paths_.unit_file, unit_maximum_bytes);
        if (contents.find("# Managed by Swapdex") == std::string::npos) {
            throw Error("unit_not_managed", "Refusing to remove a service unit not managed by Swapdex");
        }
    }
    const int disable_result = run_optional_systemctl({"--user", "disable", "--now", service_name});
    if (unit_exists && disable_result != 0 && disable_result != 127) {
        throw Error("systemctl_command_failed", "systemctl could not stop the Swapdex service");
    }
    if (unit_exists) {
        remove_unit();
    }
    if (disable_result == 0) {
        run_optional_systemctl({"--user", "daemon-reload"});
    }
    remove_installed_file(paths_.installed_executable);
    remove_installed_file(paths_.installed_asset);
    remove_installed_file(paths_.manifest_file);
    remove_legacy_desktop();
    if (purge_data) {
        if (std::filesystem::is_symlink(paths_.state_root)) {
            throw Error("purge_symlink_refused", "Refusing to purge a symbolic-link state path");
        }
        if (path_entry_exists(paths_.state_root)) {
            if (std::filesystem::is_symlink(paths_.state_root) || !std::filesystem::is_directory(paths_.state_root, error) || error) {
                throw Error("purge_path_invalid", "Refusing to purge a non-directory Swapdex state path");
            }
            std::filesystem::remove_all(paths_.state_root, error);
            if (error) {
                throw Error("purge_failed", "Unable to remove Swapdex account data");
            }
        }
        std::cout << "Swapdex data removed.\n";
    } else {
        std::cout << "Swapdex account data preserved.\n";
    }
    std::cout << "Swapdex uninstalled.\n";
    return 0;
}

}
