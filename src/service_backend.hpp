#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace swapdex {

struct ServiceRuntime {
    std::filesystem::path executable;
    std::filesystem::path asset;
    std::filesystem::path state_root;
    std::filesystem::path codex_home;
    std::filesystem::path electron_user_data;
    std::optional<std::string> data_home;
    std::optional<std::string> config_home;
    std::optional<std::string> state_home;
    std::optional<std::string> override_codex_home;
    std::optional<std::string> override_electron_user_data;
    // Tests and unusual layouts can pin the registration location. Empty means the
    // platform default.
    std::optional<std::filesystem::path> registration_override;
};

// Each supported operating system registers Swapdex to start at login in its own way.
// The lifecycle verbs stay identical so the command surface never changes.
class ServiceBackend {
public:
    using CommandRunner = std::function<int(const std::vector<std::string>&)>;

    virtual ~ServiceBackend() = default;

    virtual std::string id() const = 0;
    virtual std::filesystem::path registration_file() const = 0;
    virtual std::string registration_contents() const = 0;
    virtual bool installed() const = 0;
    virtual int enable(bool start) = 0;
    virtual int start() = 0;
    // Whether the service is running right now, so a start can be verified rather than assumed.
    virtual bool active() const = 0;
    virtual int stop() = 0;
    virtual int status() = 0;
    virtual int disable() = 0;
};

std::unique_ptr<ServiceBackend> make_service_backend(const ServiceRuntime& runtime, ServiceBackend::CommandRunner runner = {});

}
