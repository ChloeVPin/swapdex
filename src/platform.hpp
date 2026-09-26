#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace swapdex::platform {

enum class Os {
    linux,
    macos,
    windows
};

Os current_os();

// Locations. Every one of these respects the matching environment variable first so
// that a user can override the layout without changing code.
std::filesystem::path home_directory();
// The numeric user id, which launchd needs in a gui domain target.
unsigned long current_user_id();
std::filesystem::path state_directory();
std::filesystem::path data_directory();
std::filesystem::path config_directory();
std::filesystem::path cache_directory();
std::filesystem::path runtime_directory();
std::filesystem::path executable_path();
std::filesystem::path executable_directory();
std::filesystem::path default_codex_home();
std::filesystem::path default_electron_user_data();

// Application discovery. Swapdex attaches to an existing Codex install and never
// installs one, so the binary is looked up rather than assumed.
std::vector<std::filesystem::path> chatgpt_binary_candidates();
std::filesystem::path find_chatgpt_binary();
// Locates the Codex command line tool the app uses for its app server. The app moves
// its internal layout between versions, so this is resolved next to the app we already
// found, with a PATH lookup as the fallback. Empty means it could not be found.
std::filesystem::path find_codex_cli_binary();
bool matches_chatgpt_binary(const std::filesystem::path& path);
std::filesystem::path chatgpt_executable_name();
// The Codex command line binary, preferring whatever the user already has on PATH so
// terminal sessions match the version they normally use.

// Process discovery, used to refuse starting while a second Codex is already running.
std::optional<std::int64_t> running_unmanaged_chatgpt(std::int64_t managed_process_group);
// Creates a pipe with both ends close on exec. pipe2 is Linux only, so macOS builds
// the same guarantee from pipe plus fcntl.
bool make_close_on_exec_pipe(int descriptors[2]);
// Asks a normally launched app to close, then waits briefly for it to go away.
bool close_unmanaged_chatgpt();

// Durable file system helpers.
std::string read_link(const std::filesystem::path& path);
void remove_file_durable(const std::filesystem::path& path);
void sync_directory(const std::filesystem::path& path);

// Advisory single instance lock backed by the platform primitive.
class InstanceLock {
public:
    explicit InstanceLock(std::filesystem::path file);
    InstanceLock(const InstanceLock&) = delete;
    InstanceLock& operator=(const InstanceLock&) = delete;
    ~InstanceLock();

    bool acquired() const noexcept { return descriptor_ >= 0 || handle_ != nullptr; }
    int descriptor() const noexcept { return descriptor_; }

private:
    std::filesystem::path file_;
    int descriptor_ = -1;
    void* handle_ = nullptr;
};

// Child process helpers.
using EnvironmentOverrides = std::vector<std::pair<std::string, std::string>>;
std::vector<std::pair<std::string, std::string>> child_environment(const EnvironmentOverrides& overrides);
int run_command(const std::vector<std::string>& arguments);
int run_command(const std::vector<std::string>& arguments, const EnvironmentOverrides& overrides);
bool spawn_detached(const std::vector<std::string>& arguments);

;


// Registry access, only meaningful on Windows. Exposed so the service backend and the
// tests can share one implementation.
bool registry_value_exists(const std::wstring& key_path, const std::wstring& name);
std::optional<std::wstring> registry_read(const std::wstring& key_path, const std::wstring& name);
void registry_write(const std::wstring& key_path, const std::wstring& name, const std::wstring& value);
void registry_delete(const std::wstring& key_path, const std::wstring& name);

std::string to_native(const std::filesystem::path& path);
std::filesystem::path from_native(const std::string& path);

}
