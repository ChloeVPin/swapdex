#include "service_backend.hpp"

#include <iostream>
#include <sstream>
#include <string_view>

#include "platform.hpp"
#include "util.hpp"

namespace swapdex {
namespace {

constexpr std::string_view linux_service_name = "swapdex.service";
constexpr std::string_view macos_service_label = "com.swapdex.service";
constexpr std::wstring_view windows_run_key = L"HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr std::wstring_view windows_value_name = L"Swapdex";

void append_environment(std::ostringstream& unit, const char* name, const std::optional<std::string>& value) {
    if (value.has_value()) {
        unit << "Environment=" << name << "=" << *value << "\n";
    }
}

class SystemdBackend final : public ServiceBackend {
public:
    SystemdBackend(ServiceRuntime runtime, CommandRunner runner)
        : runtime_(std::move(runtime)), runner_(std::move(runner)) {}

    std::string id() const override {
        return "systemd";
    }

    std::filesystem::path registration_file() const override {
        if (runtime_.registration_override.has_value()) {
            return *runtime_.registration_override;
        }
        return platform::config_directory() / "systemd" / "user" / std::filesystem::path(std::string(linux_service_name));
    }

    std::string registration_contents() const override {
        std::ostringstream unit;
        unit << "# Managed by Swapdex\n"
             << "[Unit]\n"
             << "Description=Swapdex Codex account service\n"
             << "After=graphical-session.target\n"
             << "StartLimitIntervalSec=60\n"
             << "StartLimitBurst=3\n\n"
             << "[Service]\n"
             << "Type=simple\n"
             << "ExecStart=" << platform::to_native(runtime_.executable) << " launch\n"
             << "Restart=on-failure\n"
             << "RestartSec=5\n"
             << "KillMode=control-group\n"
             << "TimeoutStopSec=60\n"
             << "WorkingDirectory=%h\n"
             << "UMask=0077\n";
        append_environment(unit, "XDG_DATA_HOME", runtime_.data_home);
        append_environment(unit, "XDG_CONFIG_HOME", runtime_.config_home);
        append_environment(unit, "XDG_STATE_HOME", runtime_.state_home);
        append_environment(unit, "CODEX_HOME", runtime_.override_codex_home);
        append_environment(unit, "CODEX_ELECTRON_USER_DATA_PATH", runtime_.override_electron_user_data);
        unit << "\n[Install]\n"
             << "WantedBy=default.target\n";
        return unit.str();
    }

    bool installed() const override {
        std::error_code error;
        return std::filesystem::is_regular_file(registration_file(), error) && !error;
    }

    int enable(bool start) override {
        if (start) {
            return run({"--user", "enable", "--now", std::string(linux_service_name)});
        }
        return run({"--user", "enable", std::string(linux_service_name)});
    }

    int start() override {
        // Clear any earlier failed state first, otherwise the start rate limit can
        // reject an otherwise fine start. This is best effort: a unit that was never
        // failed has nothing to clear, and that must not stop the start itself.
        try {
            static_cast<void>(run({"--user", "reset-failed", std::string(linux_service_name)}));
        } catch (const std::exception&) {
        }
        return run({"--user", "start", std::string(linux_service_name)});
    }

    bool active() const override {
        return invoke({"--user", "is-active", std::string(linux_service_name)}) == 0;
    }

    int stop() override {
        return run({"--user", "stop", std::string(linux_service_name)});
    }

    int status() override {
        return invoke({"--user", "status", "--no-pager", "--full", std::string(linux_service_name)});
    }

    int disable() override {
        return run({"--user", "disable", "--now", std::string(linux_service_name)});
    }

private:
    // launchctl needs the numeric user id in a gui domain target. Omitting it gives
    // "Unrecognized target specifier", so every verb failed on macOS.
    static std::string domain_target() {
        return "gui/" + std::to_string(platform::current_user_id()) + "/" + std::string(macos_service_label);
    }

    int invoke(const std::vector<std::string>& arguments) const {
        std::vector<std::string> full;
        full.reserve(arguments.size() + 1U);
        full.emplace_back("systemctl");
        for (const std::string& argument : arguments) {
            full.push_back(argument);
        }
        return runner_ ? runner_(full) : platform::run_command(full);
    }

    int run(const std::vector<std::string>& arguments) const {
        std::vector<std::string> full;
        full.reserve(arguments.size() + 1U);
        full.emplace_back("systemctl");
        for (const std::string& argument : arguments) {
            full.push_back(argument);
        }
        const int result = runner_ ? runner_(full) : platform::run_command(full);
        if (result != 0) {
            throw Error("service_command_failed", "The system service command could not complete the requested operation");
        }
        return result;
    }

    ServiceRuntime runtime_;
    CommandRunner runner_;
};

class LaunchdBackend final : public ServiceBackend {
public:
    LaunchdBackend(ServiceRuntime runtime, CommandRunner runner)
        : runtime_(std::move(runtime)), runner_(std::move(runner)) {}

    std::string id() const override {
        return "launchd";
    }

    std::filesystem::path registration_file() const override {
        if (runtime_.registration_override.has_value()) {
            return *runtime_.registration_override;
        }
        return platform::home_directory() / "Library" / "LaunchAgents" / "com.swapdex.service.plist";
    }

    std::string registration_contents() const override {
        const std::string label(macos_service_label);
        std::ostringstream plist;
        plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
              << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
              << "<plist version=\"1.0\">\n"
              << "<dict>\n"
              << "\t<key>Label</key>\n\t<string>" << label << "</string>\n"
              << "\t<key>Comment</key>\n\t<string>Managed by Swapdex</string>\n"
              << "\t<key>ProgramArguments</key>\n\t<array>\n\t\t<string>" << platform::to_native(runtime_.executable) << "</string>\n\t\t<string>launch</string>\n\t</array>\n"
              << "\t<key>RunAtLoad</key>\n\t<true/>\n"
              << "\t<key>KeepAlive</key>\n\t<dict>\n\t\t<key>SuccessfulExit</key>\n\t\t<false/>\n\t</dict>\n"
              << "\t<key>ProcessType</key>\n\t<string>Interactive</string>\n"
              << "\t<key>LimitLoadToSessionType</key>\n\t<string>Aqua</string>\n";
        append_plist_environment(plist, "CODEX_HOME", runtime_.override_codex_home);
        append_plist_environment(plist, "CODEX_ELECTRON_USER_DATA_PATH", runtime_.override_electron_user_data);
        plist << "</dict>\n</plist>\n";
        return plist.str();
    }

    bool installed() const override {
        std::error_code error;
        return std::filesystem::is_regular_file(registration_file(), error) && !error;
    }

    int enable(bool start) override {
        const std::string target = platform::to_native(registration_file());
        const int loaded = run({"load", "-w", target});
        if (!start) {
            return loaded;
        }
        return run({"kickstart", domain_target()});
    }

    int start() override {
        return run({"kickstart", domain_target()});
    }

    bool active() const override {
        return invoke({"print", domain_target()}) == 0;
    }

    int stop() override {
        return run({"kill", "SIGTERM", domain_target()});
    }

    int status() override {
        return invoke({"print", domain_target()});
    }

    int disable() override {
        // bootout is the supported way to remove a loaded job. unload is legacy and
        // returns success even when nothing was loaded, so the result is verified
        // afterwards rather than trusted.
        const int unloaded = run({"bootout", domain_target()});
        if (unloaded == 0 && !loaded()) {
            return 0;
        }
        return unloaded == 0 ? 1 : unloaded;
    }

    // True when launchd still knows about the job, which is how a failed bootout is
    // detected instead of leaving a running service behind.
    bool loaded() const {
        return invoke({"print", domain_target()}) == 0;
    }

private:
    // launchctl needs the numeric user id in a gui domain target. Omitting it gives
    // "Unrecognized target specifier", so every verb failed on macOS.
    static std::string domain_target() {
        return "gui/" + std::to_string(platform::current_user_id()) + "/" + std::string(macos_service_label);
    }

    int invoke(const std::vector<std::string>& arguments) const {
        std::vector<std::string> full;
        full.reserve(arguments.size() + 1U);
        full.emplace_back("launchctl");
        for (const std::string& argument : arguments) {
            full.push_back(argument);
        }
        return runner_ ? runner_(full) : platform::run_command(full);
    }

    static void append_plist_environment(std::ostringstream& plist, const char* name, const std::optional<std::string>& value) {
        if (value.has_value()) {
            plist << "\t<key>EnvironmentVariables</key>\n\t<dict>\n\t\t<key>" << name << "</key>\n\t\t<string>" << *value << "</string>\n\t</dict>\n";
        }
    }

    int run(const std::vector<std::string>& arguments) const {
        std::vector<std::string> full;
        full.reserve(arguments.size() + 1U);
        full.emplace_back("launchctl");
        for (const std::string& argument : arguments) {
            full.push_back(argument);
        }
        const int result = runner_ ? runner_(full) : platform::run_command(full);
        if (result != 0) {
            throw Error("service_command_failed", "The system service command could not complete the requested operation");
        }
        return result;
    }

    ServiceRuntime runtime_;
    CommandRunner runner_;
};

class WindowsStartupBackend final : public ServiceBackend {
public:
    WindowsStartupBackend(ServiceRuntime runtime, CommandRunner runner)
        : runtime_(std::move(runtime)), runner_(std::move(runner)) {}

    std::string id() const override {
        return "windows";
    }

    std::filesystem::path registration_file() const override {
        // Registration lives in the per user Run key, so there is no file to own.
        return {};
    }

    std::string registration_contents() const override {
        return platform::to_native(runtime_.executable) + " launch";
    }

    bool installed() const override {
        return platform::registry_value_exists(std::wstring(windows_run_key), std::wstring(windows_value_name));
    }

    int enable(bool start) override {
        platform::registry_write(std::wstring(windows_run_key), std::wstring(windows_value_name), platform::from_native(registration_contents()).wstring());
        if (start) {
            return start_service();
        }
        return 0;
    }

    int start() override {
        return start_service();
    }

    // The launcher detaches, so there is nothing reliable to poll. Report the request
    // as accepted and let the service log speak for itself.
    bool active() const override {
        return true;
    }

    int stop() override {
        // The launcher owns the app process tree, so a stop request is best effort.
        return 0;
    }

    int status() override {
        if (!installed()) {
            std::cout << "Swapdex is not installed.\n";
            return 1;
        }
        std::cout << "Swapdex is registered to start at sign in.\n";
        return 0;
    }

    int disable() override {
        platform::registry_delete(std::wstring(windows_run_key), std::wstring(windows_value_name));
        return 0;
    }

private:
    int start_service() const {
        return platform::spawn_detached({platform::to_native(runtime_.executable), "launch"});
    }

    ServiceRuntime runtime_;
    CommandRunner runner_;
};

}

std::unique_ptr<ServiceBackend> make_service_backend(const ServiceRuntime& runtime, ServiceBackend::CommandRunner runner) {
    switch (platform::current_os()) {
    case platform::Os::windows:
        return std::make_unique<WindowsStartupBackend>(runtime, std::move(runner));
    case platform::Os::macos:
        return std::make_unique<LaunchdBackend>(runtime, std::move(runner));
    case platform::Os::linux:
        break;
    }
    return std::make_unique<SystemdBackend>(runtime, std::move(runner));
}

}
