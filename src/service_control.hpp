#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

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
    std::filesystem::path unit_file;
    std::filesystem::path legacy_desktop_file;
    std::filesystem::path legacy_desktop_file_fallback;
    std::filesystem::path state_root;
    std::filesystem::path lock_file;
    std::filesystem::path manifest_file;
    std::optional<std::string> xdg_data_home;
    std::optional<std::string> xdg_config_home;
    std::optional<std::string> xdg_state_home;
    std::optional<std::string> codex_home;
    std::optional<std::string> electron_user_data;
};

using SystemdCommandRunner = std::function<int(const std::vector<std::string>&)>;

ServiceControlPaths default_service_control_paths();
std::optional<RuntimePaths> installed_runtime_paths();

class ServiceControl {
public:
    explicit ServiceControl(ServiceControlPaths paths = default_service_control_paths(), SystemdCommandRunner runner = {});

    int install(bool start = true);
    int start();
    int stop();
    int status();
    int uninstall(bool purge_data = false);

private:
    int run(const std::vector<std::string>& arguments) const;
    int run_optional_systemctl(const std::vector<std::string>& arguments) const;
    void validate_paths() const;
    void validate_unit() const;
    void write_unit() const;
    void remove_legacy_desktop() const;
    void remove_installed_file(const std::filesystem::path& path) const;
    void remove_unit() const;

    ServiceControlPaths paths_;
    SystemdCommandRunner runner_;
};

}
