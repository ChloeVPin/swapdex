#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <spawn.h>
#endif

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
// A Codex process that Swapdex itself launched carries the debug pipe flag, which is
// how a managed instance left behind by a dead service is told apart from one the
// user opened by hand.
bool chatgpt_process_is_managed(std::int64_t pid);
bool process_is_alive(std::int64_t pid);
// Sends a polite termination request, or a forced one when force is set.
bool request_process_exit(std::int64_t pid, bool force);
// Asks a process to close, waits for it to go away, then forces it. Reports whether
// the process is gone.
bool close_process(std::int64_t pid);
// Closes any Codex instance Swapdex left behind (a managed orphan) and reports false
// only when a genuinely foreign app is still in the way.
bool reclaim_managed_orphans(std::int64_t owner_pid);
// Creates a wake channel used to interrupt the service wait loop. POSIX returns two
// file descriptors; Windows returns two connected sockets, which is why the ends are
// reported as intptr_t rather than int.
bool make_close_on_exec_pipe(std::intptr_t descriptors[2]);
// Asks every normally launched app to close, then waits briefly for them to go away.
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
// The same command with its output discarded, for queries whose text would only
// pollute the caller's output.
int run_command_silent(const std::vector<std::string>& arguments);
bool spawn_detached(const std::vector<std::string>& arguments);

#if defined(__APPLE__)
// posix_spawn marks the spawner as responsible for the child's TCC requests, so
// permission prompts the app raises are attributed to swapdex instead of the
// app. Disclaiming responsibility hands the prompts back to the child.
void disclaim_tcc_responsibility(posix_spawnattr_t& attributes);
#endif

;
// A detached child whose output goes to a file, so a service started at sign in
// still leaves a log behind.
bool spawn_detached(const std::vector<std::string>& arguments, const std::filesystem::path& log_file);
// Launches the Codex desktop app and reports the browser process id. Packaged
// installs can only start through package activation; other executables go through
// a normal spawn. Returns 0 on failure.
std::int64_t launch_chatgpt(const std::filesystem::path& executable, const std::vector<std::string>& arguments);


// Registry access, only meaningful on Windows. Exposed so the service backend and the
// tests can share one implementation.
bool registry_value_exists(const std::wstring& key_path, const std::wstring& name);
std::optional<std::wstring> registry_read(const std::wstring& key_path, const std::wstring& name);
void registry_write(const std::wstring& key_path, const std::wstring& name, const std::wstring& value);
void registry_delete(const std::wstring& key_path, const std::wstring& name);

std::string to_native(const std::filesystem::path& path);
std::filesystem::path from_native(const std::string& path);

// UTF-8 to UTF-16 conversions, meaningful on Windows where the process and file
// APIs are wide. Shared so every translation unit converts the same way.
std::wstring widen(const std::string& value);
std::string narrow(const std::wstring& value);

}
