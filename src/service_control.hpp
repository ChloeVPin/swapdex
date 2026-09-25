#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "service_backend.hpp"

namespace swapdex {

struct RuntimePaths {
    std::filesystem::path state_root;
    std::filesystem::path codex_home;
    std::filesystem::path electron_user_data;
};

struct ServiceControlPaths {
    std::filesystem::path source_executable;
    std::filesystem::path source_asset;
    std::filesystem::path installed_executable;
    std::filesystem::path installed_asset;
    std::filesystem::path state_root;
    std::filesystem::path lock_file;
    std::filesystem::path manifest_file;
    std::optional<std::string> xdg_data_home;
    std::optional<std::string> xdg_config_home;
    std::optional<std::string> xdg_state_home;
    std::optional<std::string> codex_home;
    std::optional<std::string> electron_user_data;
    std::optional<std::filesystem::path> registration_file;
};

ServiceControlPaths default_service_control_paths();
std::optional<RuntimePaths> installed_runtime_paths();
// Finds the renderer asset a build should install. Takes the directory holding the
// executable so a downloaded bundle, a source build, and an installed copy all work.
std::filesystem::path locate_source_asset(const std::filesystem::path& executable_directory);

// Installs the add-on for the current operating system. Each platform registers
// itself to start at sign in through its own mechanism, while the command surface
// stays identical everywhere.
// Finding and closing a normally launched app, injected so a test can describe the
// machine it wants rather than depending on what happens to be running.
struct AppProcessProbe {
    std::function<std::optional<std::int64_t>()> running_unmanaged;
    std::function<bool()> close_unmanaged;
};

class ServiceControl {
public:
    explicit ServiceControl(ServiceControlPaths paths = default_service_control_paths(), ServiceBackend::CommandRunner runner = {}, AppProcessProbe app_probe = {});

    int install(bool start = true);
    // close_app lets the command close a normally launched app that would block startup.
    int start(bool close_app = false);
    int stop();
    int status();
    int uninstall(bool purge_data = false);

private:
    ServiceRuntime runtime() const;
    void write_registration(const ServiceBackend& backend) const;
    void remove_registration(const ServiceBackend& backend) const;
    void remove_installed_file(const std::filesystem::path& path) const;
    void validate_sources() const;

    ServiceControlPaths paths_;
    ServiceBackend::CommandRunner runner_;
    AppProcessProbe app_probe_;
};

}
