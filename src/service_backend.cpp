#include "service_backend.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

// The last few service log lines are the first thing a failing start needs, so
// status surfaces them rather than leaving them in a file nobody knows about.
void tail_service_log(const std::filesystem::path& log) {
    std::error_code error;
    const auto size = std::filesystem::file_size(log, error);
    if (error || size == 0) {
        return;
    }
    std::ifstream stream(log, std::ios::binary);
    if (!stream) {
        return;
    }
    constexpr std::streamoff window = 16384;
    if (size > window) {
        stream.seekg(size - window);
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    std::string text = buffer.str();
    // Skip a partial first line when the tail was cut mid line.
    if (size > window) {
        const std::size_t newline = text.find('\n');
        text = newline == std::string::npos ? "" : text.substr(newline + 1);
    }
    if (text.empty()) {
        return;
    }
    std::cout << "Recent service log (" << log.string() << "):\n" << text;
    if (text.back() != '\n') {
        std::cout << "\n";
    }
}

// The service holds its lock for its whole life, so the lock is the honest
// running signal on every platform where no service manager tracks the process.
bool service_lock_held(const std::filesystem::path& state_root) {
    std::error_code error;
    if (!std::filesystem::is_directory(state_root, error) || error) {
        return false;
    }
    platform::InstanceLock probe(state_root / "service.lock");
    return !probe.acquired();
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
        const std::string log = platform::to_native(runtime_.state_root / "service.log");
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
              << "\t<key>LimitLoadToSessionType</key>\n\t<string>Aqua</string>\n"
              << "\t<key>StandardOutPath</key>\n\t<string>" << log << "</string>\n"
              << "\t<key>StandardErrorPath</key>\n\t<string>" << log << "</string>\n";
        append_plist_environment(plist);
        plist << "</dict>\n</plist>\n";
        return plist.str();
    }

    bool installed() const override {
        std::error_code error;
        return std::filesystem::is_regular_file(registration_file(), error) && !error;
    }

    int enable(bool start) override {
        const std::string target = platform::to_native(registration_file());
        if (!start) {
            // Registered means the file exists; --no-start must not leave the job
            // loaded because RunAtLoad would launch it anyway.
            if (loaded()) {
                return run({"bootout", domain_target()});
            }
            return 0;
        }
        // A load already starts the job because RunAtLoad is set, so kicking it again
        // here killed the first copy mid launch and could leave an orphaned app.
        if (!loaded()) {
            return run({"load", "-w", target});
        }
        return run({"kickstart", domain_target()});
    }

    int start() override {
        // A loaded job that is not running needs a kick; one that is not loaded needs
        // a load, which starts it through RunAtLoad.
        if (loaded()) {
            return run({"kickstart", domain_target()});
        }
        return run({"load", "-w", platform::to_native(registration_file())});
    }

    bool active() const override {
        // launchd only knows the job is loaded, not whether the process is alive, so
        // asking it after a stop answered "running" forever. The service holds its
        // lock for its whole life, so the lock is the honest signal.
        std::error_code error;
        if (!std::filesystem::is_directory(runtime_.state_root, error) || error) {
            return false;
        }
        platform::InstanceLock probe(runtime_.state_root / "service.lock");
        return !probe.acquired();
    }

    int stop() override {
        // kill only signals the process and the job stays loaded, where KeepAlive can
        // revive it again. bootout unloads the job; the registration file stays on
        // disk so a start loads it back.
        if (!loaded()) {
            return 0;
        }
        const int result = run({"bootout", domain_target()});
        wait_unloaded();
        return result;
    }

    int status() override {
        if (active()) {
            std::cout << "Swapdex is running.\n";
            tail_service_log(runtime_.state_root / "service.log");
            return 0;
        }
        if (loaded()) {
            std::cout << "Swapdex is loaded but not running. Run swapdex start to bring it up.\n";
        } else {
            std::cout << "Swapdex is installed but not loaded. Run swapdex start to bring it up.\n";
        }
        tail_service_log(runtime_.state_root / "service.log");
        return 1;
    }

    int disable() override {
        // bootout is the supported way to remove a loaded job, and it is an error on
        // a job that is not loaded at all. Checking first keeps a reinstall over a
        // stopped service quiet instead of printing a failed bootout.
        if (!loaded()) {
            return 0;
        }
        // unload is legacy and returns success even when nothing was loaded, so the
        // result is verified afterwards rather than trusted.
        const int unloaded = run({"bootout", domain_target()});
        wait_unloaded();
        if (unloaded == 0 && !loaded()) {
            return 0;
        }
        return unloaded == 0 ? 1 : unloaded;
    }

    // bootout only queues the SIGTERM and the job object survives until the process
    // finishes exiting. A load issued during that teardown is silently dropped, so
    // callers must wait for the job to actually disappear before registering again.
    void wait_unloaded() const {
        for (int i = 0; i < 100 && loaded(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
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
        // Queries must not print: launchctl print dumps the whole service record,
        // which is how raw internals ended up in the output of start and status.
        return runner_ ? runner_(full) : platform::run_command_silent(full);
    }

    void append_plist_environment(std::ostringstream& plist) const {
        // A plist dict cannot repeat a key, so every override goes into one
        // EnvironmentVariables block rather than one block per variable.
        std::vector<std::pair<std::string, std::string>> entries;
        if (runtime_.override_codex_home.has_value()) {
            entries.emplace_back("CODEX_HOME", *runtime_.override_codex_home);
        }
        if (runtime_.override_electron_user_data.has_value()) {
            entries.emplace_back("CODEX_ELECTRON_USER_DATA_PATH", *runtime_.override_electron_user_data);
        }
        if (entries.empty()) {
            return;
        }
        plist << "\t<key>EnvironmentVariables</key>\n\t<dict>\n";
        for (const auto& [name, value] : entries) {
            plist << "\t\t<key>" << name << "</key>\n\t\t<string>" << value << "</string>\n";
        }
        plist << "\t</dict>\n";
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
        // The Run key value is a command line, so the executable needs quoting to
        // survive a path with spaces.
        return "\"" + platform::to_native(runtime_.executable) + "\" launch";
    }

    bool installed() const override {
        return platform::registry_value_exists(std::wstring(windows_run_key), registry_value_name());
    }

    int enable(bool start) override {
        platform::registry_write(std::wstring(windows_run_key), registry_value_name(), platform::from_native(registration_contents()).wstring());
        if (start) {
            return start_service();
        }
        return 0;
    }

    int start() override {
        return start_service();
    }

    // The Run key only says the service starts at sign in. The service holds its
    // lock for its whole life, so the lock is the honest signal.
    bool active() const override {
        return service_lock_held(runtime_.state_root);
    }

    int stop() override {
#if defined(_WIN32)
        if (runner_) {
            // Tests model the service manager through the runner; honor it the
            // same way start_service does.
            return runner_({platform::to_native(runtime_.executable), "stop"});
        }
        const HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\SwapdexServiceStop");
        if (event == nullptr) {
            return active() ? 1 : 0;
        }
        SetEvent(event);
        CloseHandle(event);
        for (int i = 0; i < 150 && active(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return active() ? 1 : 0;
#else
        return 0;
#endif
    }

    int status() override {
        if (active()) {
            std::cout << "Swapdex is running.\n";
            tail_service_log(runtime_.state_root / "service.log");
            return 0;
        }
        if (!installed()) {
            std::cout << "Swapdex is not installed.\n";
            return 1;
        }
        std::cout << "Swapdex is registered to start at sign in but not running. Run swapdex start to bring it up.\n";
        tail_service_log(runtime_.state_root / "service.log");
        return 1;
    }

    int disable() override {
        // Take the running service down first so a stopped registration cannot
        // leave a live instance behind.
        if (active()) {
            static_cast<void>(stop());
        }
        platform::registry_delete(std::wstring(windows_run_key), registry_value_name());
        return 0;
    }

private:
    // Tests pass a registration_override (a file path for the file based backends);
    // on Windows the registry is the registry, so the override lends its file name
    // to the Run value instead. That keeps a test registration well away from a
    // real Swapdex entry.
    std::wstring registry_value_name() const {
        if (runtime_.registration_override.has_value()) {
            const std::filesystem::path name = runtime_.registration_override->filename();
            if (!name.empty()) {
                return name.wstring();
            }
        }
        return std::wstring(windows_value_name);
    }

    int start_service() const {
        if (runner_) {
            return runner_({platform::to_native(runtime_.executable), "launch"});
        }
        const std::filesystem::path log = runtime_.state_root / "service.log";
        std::error_code error;
        std::filesystem::create_directories(runtime_.state_root, error);
        return platform::spawn_detached({platform::to_native(runtime_.executable), "launch"}, log) ? 0 : 1;
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
