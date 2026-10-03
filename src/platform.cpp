#include "platform.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <thread>

#if !defined(_WIN32)
#include <sys/select.h>
#include <termios.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
// Linux and macOS share the same base headers. Only the extras differ, so they are
// added on top rather than replacing the shared set.
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#else
#include <sys/file.h>
#endif
#endif

namespace swapdex::platform {
namespace {

// execvpe is a GNU extension and macOS does not have it. Resolve the program against
// PATH ourselves and use execve, which every platform here has.
void exec_program(const char* program, char* const arguments[], char* const environment[]) {
#if defined(__linux__) || defined(__CYGWIN__)
    ::execvpe(program, arguments, environment);
    ::_exit(127);
#else
    const std::filesystem::path requested(program);
    if (requested.has_parent_path() && requested.is_absolute()) {
        ::execve(program, arguments, environment);
        ::_exit(127);
    }
    const char* path_variable = std::getenv("PATH");
    if (path_variable != nullptr) {
        const std::string entries(path_variable);
        std::size_t start = 0;
        while (start <= entries.size()) {
            const std::size_t end = entries.find(':', start);
            const std::string directory = entries.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!directory.empty()) {
                std::error_code error;
                const std::filesystem::path candidate = std::filesystem::path(directory) / (requested.has_parent_path() ? requested : requested.filename());
                if (std::filesystem::is_regular_file(candidate, error) && !error) {
                    ::execve(candidate.c_str(), arguments, environment);
                    ::_exit(127);
                }
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
    ::_exit(127);
#endif
}

}

namespace {

std::optional<std::string> environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

#if defined(_WIN32)

std::wstring widen(const std::string& value) {
    if (value.empty()) {
        return std::wstring();
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return std::wstring();
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), required);
    return result;
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) {
        return std::string();
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::string();
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
}

std::optional<std::string> windows_environment(const char* name) {
    const std::wstring key = widen(name);
    const DWORD length = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
    if (length == 0) {
        return std::nullopt;
    }
    std::wstring buffer(length, L'\0');
    const DWORD written = GetEnvironmentVariableW(key.c_str(), buffer.data(), length);
    if (written == 0 || written >= length) {
        return std::nullopt;
    }
    buffer.resize(written);
    return narrow(buffer);
}

std::optional<std::string> first_existing(const std::vector<std::filesystem::path>& candidates) {
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate.string();
        }
    }
    return std::nullopt;
}

#else

std::optional<std::string> first_existing(const std::vector<std::filesystem::path>& candidates) {
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return candidate.string();
        }
    }
    return std::nullopt;
}

#endif

std::vector<std::string> allowed_child_variables() {
    return {
        "PATH", "HOME", "USER", "LOGNAME", "SHELL", "TMPDIR", "TMP", "TEMP",
        "LANG", "LC_ALL", "LC_CTYPE", "DISPLAY", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR",
        "XDG_SESSION_TYPE", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_STATE_HOME",
        "XDG_CACHE_HOME", "DBUS_SESSION_BUS_ADDRESS", "DESKTOP_SESSION",
        "GTK_IM_MODULE", "QT_IM_MODULE", "LD_LIBRARY_PATH", "SystemRoot", "SystemDrive",
        "windir", "COMSPEC", "PATHEXT", "USERPROFILE", "APPDATA", "LOCALAPPDATA",
        "PROGRAMFILES", "PROGRAMFILES(X86)", "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE",
    };
}

}

Os current_os() {
#if defined(_WIN32)
    return Os::windows;
#elif defined(__APPLE__)
    return Os::macos;
#else
    return Os::linux;
#endif
}

std::string to_native(const std::filesystem::path& path) {
#if defined(_WIN32)
    return narrow(path.wstring());
#else
    return path.string();
#endif
}

std::filesystem::path from_native(const std::string& path) {
#if defined(_WIN32)
    return std::filesystem::path(widen(path));
#else
    return std::filesystem::path(path);
#endif
}

std::filesystem::path home_directory() {
    if (const auto value = environment("HOME"); value.has_value()) {
        return from_native(*value);
    }
#if defined(_WIN32)
    if (const auto value = windows_environment("USERPROFILE"); value.has_value()) {
        return from_native(*value);
    }
    if (const auto drive = windows_environment("HOMEDRIVE"); drive.has_value()) {
        if (const auto path = windows_environment("HOMEPATH"); path.has_value()) {
            return from_native(*drive + *path);
        }
    }
#else
    if (const struct passwd* entry = getpwuid(getuid()); entry != nullptr && entry->pw_dir != nullptr) {
        return from_native(entry->pw_dir);
    }
#endif
    return std::filesystem::current_path();
}

std::filesystem::path state_directory() {
#if defined(_WIN32)
    if (const auto value = windows_environment("LOCALAPPDATA"); value.has_value()) {
        return from_native(*value);
    }
    return home_directory() / "AppData" / "Local";
#elif defined(__APPLE__)
    if (const auto value = environment("XDG_STATE_HOME"); value.has_value()) {
        return from_native(*value);
    }
    return home_directory() / "Library" / "Application Support";
#else
    if (const auto value = environment("XDG_STATE_HOME"); value.has_value()) {
        return from_native(*value);
    }
    return home_directory() / ".local" / "state";
#endif
}

std::filesystem::path data_directory() {
#if defined(_WIN32)
    if (const auto value = windows_environment("LOCALAPPDATA"); value.has_value()) {
        return from_native(*value);
    }
    return home_directory() / "AppData" / "Local";
#elif defined(__APPLE__)
    if (const auto value = environment("XDG_DATA_HOME"); value.has_value()) {
        return from_native(*value);
    }
    return home_directory() / "Library" / "Application Support";
#else
    if (const auto value = environment("XDG_DATA_HOME"); value.has_value()) {
        return from_native(*value);
    }
    return home_directory() / ".local" / "share";
#endif
}

std::filesystem::path config_directory() {
#if defined(_WIN32)
    if (const auto value = windows_environment("APPDATA"); value.has_value()) {
        return from_native(*value);
    }
    return home_directory() / "AppData" / "Roaming";
#elif defined(__APPLE__)
    if (const auto value = environment("XDG_CONFIG_HOME"); value.has_value()) {
        return from_native(*value);
    }
    return home_directory() / "Library" / "Application Support";
#else
    if (const auto value = environment("XDG_CONFIG_HOME"); value.has_value()) {
        return from_native(*value);
    }
    return home_directory() / ".config";
#endif
}

std::filesystem::path cache_directory() {
#if defined(_WIN32)
    if (const auto value = windows_environment("LOCALAPPDATA"); value.has_value()) {
        return from_native(*value);
    }
    return home_directory() / "AppData" / "Local";
#elif defined(__APPLE__)
    if (const auto value = environment("XDG_CACHE_HOME"); value.has_value()) {
        return from_native(*value);
    }
    return home_directory() / "Library" / "Caches";
#else
    if (const auto value = environment("XDG_CACHE_HOME"); value.has_value()) {
        return from_native(*value);
    }
    return home_directory() / ".cache";
#endif
}

std::filesystem::path runtime_directory() {
#if defined(_WIN32)
    if (const auto value = windows_environment("TEMP"); value.has_value()) {
        return from_native(*value);
    }
    return cache_directory();
#elif defined(__APPLE__)
    if (const auto value = environment("TMPDIR"); value.has_value()) {
        return from_native(*value);
    }
    return std::filesystem::temp_directory_path();
#else
    if (const auto value = environment("XDG_RUNTIME_DIR"); value.has_value()) {
        return from_native(*value);
    }
    return std::filesystem::temp_directory_path();
#endif
}

std::filesystem::path executable_path() {
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH] = {0};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::filesystem::path(std::wstring(buffer, length));
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size > 0) {
        std::string buffer(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
            std::error_code resolve_error;
            const std::filesystem::path canonical = std::filesystem::canonical(std::filesystem::path(buffer.c_str()), resolve_error);
            return resolve_error ? std::filesystem::path(buffer.c_str()) : canonical;
        }
    }
#else
    const std::string link = read_link("/proc/self/exe");
    if (!link.empty()) {
        return std::filesystem::path(link);
    }
#endif
    if (const auto value = environment("HOME"); value.has_value()) {
        return home_directory() / ".local" / "bin" / "swapdex";
    }
    return std::filesystem::current_path() / "swapdex";
}

std::filesystem::path executable_directory() {
    return executable_path().parent_path();
}

std::filesystem::path default_codex_home() {
    if (const auto value = environment("CODEX_HOME"); value.has_value()) {
        return from_native(*value);
    }
    return home_directory() / ".codex";
}

std::filesystem::path default_electron_user_data() {
    if (const auto value = environment("CODEX_ELECTRON_USER_DATA_PATH"); value.has_value()) {
        return from_native(*value);
    }
#if defined(_WIN32)
    return config_directory() / "Codex";
#elif defined(__APPLE__)
    return home_directory() / "Library" / "Application Support" / "Codex";
#else
    return config_directory() / "Codex";
#endif
}

std::filesystem::path chatgpt_executable_name() {
#if defined(_WIN32)
    return "Codex.exe";
#elif defined(__APPLE__)
    return "Codex";
#else
    return "ChatGPT";
#endif
}

std::vector<std::filesystem::path> chatgpt_binary_candidates() {
    std::vector<std::filesystem::path> candidates;
    if (const auto value = environment("SWAPDEX_CHATGPT_BINARY"); value.has_value()) {
        candidates.push_back(from_native(*value));
    }
#if defined(_WIN32)
    if (const auto local = windows_environment("LOCALAPPDATA"); local.has_value()) {
        const std::filesystem::path base = from_native(*local);
        candidates.push_back(base / "Programs" / "Codex" / "Codex.exe");
        candidates.push_back(base / "Programs" / "ChatGPT" / "ChatGPT.exe");
    }
    if (const auto programs = windows_environment("PROGRAMFILES"); programs.has_value()) {
        candidates.push_back(from_native(*programs) / "Codex" / "Codex.exe");
    }
    if (const auto programs = windows_environment("PROGRAMFILES(X86)"); programs.has_value()) {
        candidates.push_back(from_native(*programs) / "Codex" / "Codex.exe");
    }
#elif defined(__APPLE__)
    candidates.push_back(std::filesystem::path("/Applications") / "Codex.app" / "Contents" / "MacOS" / "Codex");
    candidates.push_back(std::filesystem::path("/Applications") / "ChatGPT.app" / "Contents" / "MacOS" / "ChatGPT");
    const std::filesystem::path applications = home_directory() / "Applications";
    candidates.push_back(applications / "Codex.app" / "Contents" / "MacOS" / "Codex");
    candidates.push_back(applications / "ChatGPT.app" / "Contents" / "MacOS" / "ChatGPT");
#else
    candidates.push_back(std::filesystem::path("/usr/lib/chatgpt/ChatGPT"));
    candidates.push_back(std::filesystem::path("/opt/chatgpt/ChatGPT"));
    candidates.push_back(std::filesystem::path("/usr/bin/chatgpt"));
    candidates.push_back(std::filesystem::path("/opt/Codex/Codex"));
    candidates.push_back(std::filesystem::path("/usr/local/bin/Codex"));
#endif
    return candidates;
}

std::filesystem::path find_chatgpt_binary() {
    const std::vector<std::filesystem::path> candidates = chatgpt_binary_candidates();
    if (const auto found = first_existing(candidates); found.has_value()) {
        return from_native(*found);
    }
    return {};
}

namespace {

std::vector<std::filesystem::path> path_directories() {
    std::vector<std::filesystem::path> directories;
    const std::optional<std::string> value = environment("PATH");
    if (!value.has_value()) {
        return directories;
    }
    const std::string entries(*value);
    std::size_t start = 0;
    while (start <= entries.size()) {
        const std::size_t end = entries.find(':', start);
        const std::string piece = entries.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!piece.empty()) {
            directories.push_back(from_native(piece));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return directories;
}

std::vector<std::filesystem::path> executable_names() {
#if defined(_WIN32)
    return {"codex.exe", "codex.cmd"};
#else
    return {"codex"};
#endif
}

}

std::filesystem::path find_codex_cli_binary() {
    if (const auto value = environment("SWAPDEX_CODEX_CLI_BINARY"); value.has_value()) {
        const std::filesystem::path override_path = from_native(*value);
        std::error_code error;
        if (std::filesystem::is_regular_file(override_path, error) && !error) {
            return std::filesystem::absolute(override_path).lexically_normal();
        }
    }
    // Prefer the copy the app itself ships, resolved relative to the app so a layout
    // change inside the bundle is followed rather than hardcoded. The app is a macOS
    // bundle on one platform and a plain directory tree on another, so both shapes are
    // searched, and the app's own copy always wins over whatever is on PATH.
    const std::filesystem::path app = find_chatgpt_binary();
    if (!app.empty()) {
        const std::filesystem::path parent = app.parent_path();
        std::vector<std::filesystem::path> roots;
        if (parent.filename() == "MacOS") {
            roots.push_back(parent.parent_path() / "Resources");
        } else {
            roots.push_back(parent / "resources");
            roots.push_back(parent);
        }
        const std::vector<std::filesystem::path> layouts = {
            std::filesystem::path("codex-cli") / "CodexCLI.app" / "Contents" / "MacOS" / "codex",
            std::filesystem::path("codex-cli") / "bin" / "codex",
            std::filesystem::path("codex"),
            std::filesystem::path("bin") / "codex",
        };
        for (const std::filesystem::path& root : roots) {
            for (const std::filesystem::path& layout : layouts) {
                const std::filesystem::path candidate = root / layout;
                std::error_code error;
                if (std::filesystem::is_regular_file(candidate, error) && !error) {
                    return std::filesystem::absolute(candidate).lexically_normal();
                }
            }
        }
    }
    // Fall back to whatever is on PATH.
    for (const std::filesystem::path& directory : path_directories()) {
        for (const std::filesystem::path& name : executable_names()) {
            const std::filesystem::path candidate = directory / name;
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error) && !error) {
                return std::filesystem::absolute(candidate).lexically_normal();
            }
        }
    }
    return {};
}

bool matches_chatgpt_binary(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(path, error);
    const std::filesystem::path target = error ? path : resolved;
    const std::string expected_name = chatgpt_executable_name().string();
    if (target.filename().string() == expected_name) {
        return true;
    }
#if !defined(_WIN32)
    for (const auto& candidate : chatgpt_binary_candidates()) {
        const std::filesystem::path resolved_candidate = std::filesystem::weakly_canonical(candidate, error);
        if (!error && resolved_candidate == target) {
            return true;
        }
    }
    return path == expected_name;
#else
    return false;
#endif
}

std::string read_link(const std::filesystem::path& path) {
#if defined(_WIN32)
    static_cast<void>(path);
    return {};
#else
    std::array<char, 4096> buffer{};
    const ssize_t count = ::readlink(path.c_str(), buffer.data(), buffer.size() - 1U);
    if (count <= 0) {
        return {};
    }
    return std::string(buffer.data(), static_cast<std::size_t>(count));
#endif
}

void sync_directory(const std::filesystem::path& path) {
#if defined(_WIN32)
    static_cast<void>(path);
#else
#if defined(__linux__)
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY);
#endif
    if (descriptor < 0) {
        return;
    }
    ::fsync(descriptor);
    ::close(descriptor);
#endif
}

void remove_file_durable(const std::filesystem::path& path) {
#if defined(_WIN32)
    const std::wstring native = path.wstring();
    if (DeleteFileW(native.c_str()) == 0 && GetLastError() != ERROR_FILE_NOT_FOUND) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Unable to remove a Swapdex file");
    }
#else
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
        throw std::system_error(errno, std::generic_category(), "Unable to remove a Swapdex file");
    }
    sync_directory(path.parent_path());
#endif
}

InstanceLock::InstanceLock(std::filesystem::path file)
    : file_(std::move(file)) {
    const std::filesystem::path parent = file_.parent_path();
#if defined(_WIN32)
    const std::wstring native = file_.wstring();
    HANDLE handle = CreateFileW(native.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    OVERLAPPED overlapped{};
    if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped) == 0) {
        CloseHandle(handle);
        return;
    }
    handle_ = handle;
#else
    const int descriptor = ::open(file_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
        return;
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        ::close(descriptor);
        return;
    }
    (void)parent;
    descriptor_ = descriptor;
#endif
}

InstanceLock::~InstanceLock() {
#if defined(_WIN32)
    if (handle_ != nullptr) {
        OVERLAPPED overlapped{};
        UnlockFileEx(static_cast<HANDLE>(handle_), 0, 1, 0, &overlapped);
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
#else
    if (descriptor_ >= 0) {
        ::flock(descriptor_, LOCK_UN);
        ::close(descriptor_);
        descriptor_ = -1;
    }
#endif
}

std::vector<std::pair<std::string, std::string>> child_environment(const EnvironmentOverrides& overrides) {
    std::vector<std::pair<std::string, std::string>> result;
    const std::vector<std::string> allowed = allowed_child_variables();
    for (const std::string& name : allowed) {
        if (const auto value = environment(name.c_str()); value.has_value()) {
            result.emplace_back(name, *value);
        }
    }
    for (const auto& [name, value] : overrides) {
        const auto existing = std::find_if(result.begin(), result.end(), [&name](const auto& entry) { return entry.first == name; });
        if (existing == result.end()) {
            result.emplace_back(name, value);
        } else {
            existing->second = value;
        }
    }
    return result;
}

#if defined(__APPLE__)
// Private but present across macOS releases; weak linked so the build keeps
// working if it ever disappears.
extern "C" int responsibility_spawnattrs_setdisclaim(posix_spawnattr_t*, int) __attribute__((weak_import));

void disclaim_tcc_responsibility(posix_spawnattr_t& attributes) {
    if (responsibility_spawnattrs_setdisclaim != nullptr) {
        responsibility_spawnattrs_setdisclaim(&attributes, 1);
    }
}
#endif

int run_command(const std::vector<std::string>& arguments) {
    return run_command(arguments, {});
}

int run_command_silent(const std::vector<std::string>& arguments) {
#if defined(_WIN32)
    return run_command(arguments, {});
#else
    if (arguments.empty()) {
        return 1;
    }
    std::vector<std::string> storage;
    for (const auto& entry : child_environment({})) {
        storage.push_back(entry.first + "=" + entry.second);
        storage.emplace_back();
    }
    std::vector<char*> raw;
    raw.reserve(storage.size());
    for (std::string& entry : storage) {
        raw.push_back(entry.data());
    }
    raw.push_back(nullptr);

    std::vector<std::string> argument_storage;
    argument_storage.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        argument_storage.push_back(argument);
    }
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(argument_storage.size() + 1U);
    for (std::string& argument : argument_storage) {
        argument_pointers.push_back(argument.data());
    }
    argument_pointers.push_back(nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
        return 1;
    }
    if (child == 0) {
        const int null_descriptor = ::open("/dev/null", O_RDWR);
        if (null_descriptor >= 0) {
            ::dup2(null_descriptor, STDOUT_FILENO);
            ::dup2(null_descriptor, STDERR_FILENO);
            if (null_descriptor > STDERR_FILENO) {
                ::close(null_descriptor);
            }
        }
        exec_program(argument_pointers.front(), argument_pointers.data(), raw.data());
    }
    int status = 0;
    if (::waitpid(child, &status, 0) < 0) {
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
#endif
}

int run_command(const std::vector<std::string>& arguments, const EnvironmentOverrides& overrides) {
    if (arguments.empty()) {
        return 1;
    }
#if defined(_WIN32)
    std::wstring command_line;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index > 0) {
            command_line.push_back(L' ');
        }
        command_line.append(widen(arguments[index]));
    }
    std::vector<std::pair<std::string, std::string>> environment_pairs = child_environment(overrides);
    std::wstring environment_block;
    for (const auto& [name, value] : environment_pairs) {
        environment_block.append(widen(name));
        environment_block.push_back(L'=');
        environment_block.append(widen(value));
        environment_block.push_back(L'\0');
    }
    environment_block.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    if (CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, 0, environment_block.empty() ? nullptr : environment_block.data(), nullptr, &startup, &process) == 0) {
        return 1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    std::vector<std::string> storage;
    for (const auto& entry : child_environment(overrides)) {
        storage.push_back(entry.first + "=" + entry.second);
        storage.emplace_back();
    }
    std::vector<char*> raw;
    raw.reserve(storage.size());
    for (std::string& entry : storage) {
        raw.push_back(entry.data());
    }
    raw.push_back(nullptr);

    std::vector<std::string> argument_storage;
    argument_storage.reserve(arguments.size());
    for (const std::string& argument : arguments) {
        argument_storage.push_back(argument);
    }
    std::vector<char*> argument_pointers;
    argument_pointers.reserve(argument_storage.size() + 1U);
    for (std::string& argument : argument_storage) {
        argument_pointers.push_back(argument.data());
    }
    argument_pointers.push_back(nullptr);

    const pid_t child = ::fork();
    if (child < 0) {
        return 1;
    }
    if (child == 0) {
        exec_program(argument_pointers.front(), argument_pointers.data(), raw.data());
    }
    int status = 0;
    if (::waitpid(child, &status, 0) < 0) {
        return 1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
#endif
}

bool spawn_detached(const std::vector<std::string>& arguments) {
    if (arguments.empty()) {
        return false;
    }
#if defined(_WIN32)
    std::wstring command_line;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index > 0) {
            command_line.push_back(L' ');
        }
        command_line.append(widen(arguments[index]));
    }
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process) == 0) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    const pid_t child = ::fork();
    if (child < 0) {
        return false;
    }
    if (child == 0) {
        ::setsid();
        std::vector<std::string> storage;
        for (const auto& entry : child_environment({})) {
            storage.push_back(entry.first + "=" + entry.second);
            storage.emplace_back();
        }
        std::vector<char*> raw;
        for (std::string& entry : storage) {
            raw.push_back(entry.data());
        }
        raw.push_back(nullptr);
        std::vector<std::string> argument_storage = arguments;
        std::vector<char*> pointers;
        for (std::string& argument : argument_storage) {
            pointers.push_back(argument.data());
        }
        pointers.push_back(nullptr);
        const int null_descriptor = ::open("/dev/null", O_RDWR);
        if (null_descriptor >= 0) {
            ::dup2(null_descriptor, STDIN_FILENO);
            ::dup2(null_descriptor, STDOUT_FILENO);
            ::dup2(null_descriptor, STDERR_FILENO);
            if (null_descriptor > STDERR_FILENO) {
                ::close(null_descriptor);
            }
        }
        exec_program(pointers.front(), pointers.data(), raw.data());
    }
    return true;
#endif
}

bool request_process_exit(std::int64_t pid, bool force) {
#if defined(_WIN32)
    HANDLE handle = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return false;
    }
    const BOOL ok = TerminateProcess(handle, force ? 1 : 0);
    CloseHandle(handle);
    return ok != FALSE;
#else
    return ::kill(static_cast<pid_t>(pid), force ? SIGKILL : SIGTERM) == 0;
#endif
}

bool process_is_alive(std::int64_t pid) {
#if defined(_WIN32)
    HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr) {
        return false;
    }
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(handle, &code) != FALSE && code == STILL_ACTIVE;
    CloseHandle(handle);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

bool close_process(std::int64_t pid) {
    // The app takes a few seconds to shut down cleanly, and reporting failure because
    // it had not finished yet was wrong: the app really was closing. Wait patiently
    // before insisting, and only then force it.
    request_process_exit(pid, false);
    for (int attempt = 0; attempt < 150; ++attempt) {
        if (!process_is_alive(pid)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    request_process_exit(pid, true);
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (!process_is_alive(pid)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool close_unmanaged_chatgpt() {
    // Several Codex processes can be open at once, so closing only the first left
    // the rest to block the service.
    bool clear = true;
    for (int guard = 0; guard < 8; ++guard) {
        const std::optional<std::int64_t> pid = running_unmanaged_chatgpt(-1);
        if (!pid.has_value()) {
            return clear;
        }
        if (!close_process(*pid)) {
            clear = false;
        }
    }
    return clear && !running_unmanaged_chatgpt(-1).has_value();
}

bool chatgpt_process_is_managed(std::int64_t pid) {
    // Swapdex spawns the app with --remote-debugging-pipe, a flag nothing else
    // passes, so its presence in the arguments identifies a managed instance left
    // behind when its service died.
#if defined(_WIN32)
    static_cast<void>(pid);
    return false;
#elif defined(__APPLE__)
    const std::string command = "ps -o args= -p " + std::to_string(pid) + " 2>/dev/null";
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return false;
    }
    std::string arguments;
    std::array<char, 4096> buffer{};
    while (::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        arguments.append(buffer.data());
    }
    ::pclose(pipe);
    return arguments.find("--remote-debugging-pipe") != std::string::npos;
#else
    std::ifstream stream("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
    if (!stream) {
        return false;
    }
    const std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return contents.find("--remote-debugging-pipe") != std::string::npos;
#endif
}

bool reclaim_managed_orphans(std::int64_t owner_pid) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const std::optional<std::int64_t> pid = running_unmanaged_chatgpt(owner_pid);
        if (!pid.has_value()) {
            return true;
        }
        if (!chatgpt_process_is_managed(*pid)) {
            return false;
        }
        close_process(*pid);
    }
    return !running_unmanaged_chatgpt(owner_pid).has_value();
}

bool make_close_on_exec_pipe(int descriptors[2]) {
    descriptors[0] = -1;
    descriptors[1] = -1;
#if defined(__linux__)
    return ::pipe2(descriptors, O_CLOEXEC) == 0;
#else
    if (::pipe(descriptors) != 0) {
        return false;
    }
    for (const int descriptor : {descriptors[0], descriptors[1]}) {
        const int flags = ::fcntl(descriptor, F_GETFD);
        if (flags == -1 || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == -1) {
            ::close(descriptors[0]);
            ::close(descriptors[1]);
            return false;
        }
    }
    return true;
#endif
}

unsigned long current_user_id() {
#if defined(_WIN32)
    return 0U;
#else
    return static_cast<unsigned long>(::getuid());
#endif
}

std::optional<std::int64_t> running_unmanaged_chatgpt(std::int64_t managed_process_group) {
#if defined(_WIN32)
    const DWORD snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    std::optional<std::int64_t> result;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (wstring(entry.szExeFile) == L"Codex.exe" || wstring(entry.szExeFile) == L"ChatGPT.exe") {
                const std::int64_t group = static_cast<std::int64_t>(entry.th32ParentProcessID);
                if (group != managed_process_group) {
                    result = static_cast<std::int64_t>(entry.th32ProcessID);
                    break;
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
#elif defined(__APPLE__)
    // macOS has no proc filesystem, so the process list comes from libproc.
    const int capacity = 4096;
    std::vector<pid_t> pids(static_cast<std::size_t>(capacity));
    const int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), capacity * static_cast<int>(sizeof(pid_t)));
    if (bytes <= 0) {
        return std::nullopt;
    }
    const int count = bytes / static_cast<int>(sizeof(pid_t));
    for (int index = 0; index < count; ++index) {
        const pid_t pid = pids[static_cast<std::size_t>(index)];
        if (pid <= 0) {
            continue;
        }
        std::vector<char> path(static_cast<std::size_t>(PROC_PIDPATHINFO_MAXSIZE));
        if (proc_pidpath(pid, path.data(), static_cast<std::uint32_t>(path.size())) <= 0) {
            continue;
        }
        if (!matches_chatgpt_binary(from_native(std::string(path.data())))) {
            continue;
        }
        proc_bsdinfo info{};
        if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info)) != sizeof(info)) {
            continue;
        }
        if (managed_process_group >= 0 && static_cast<std::int64_t>(info.pbi_ppid) == managed_process_group) {
            continue;
        }
        return static_cast<std::int64_t>(pid);
    }
    return std::nullopt;
#else
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        if (error) {
            return std::nullopt;
        }
        const std::string name = entry.path().filename().string();
        if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())) == 0) {
            continue;
        }
        const std::filesystem::path exe = entry.path() / "exe";
        const std::string link = read_link(exe);
        if (link.empty() || !matches_chatgpt_binary(from_native(link))) {
            continue;
        }
        std::ifstream stat_stream((entry.path() / "stat").string());
        std::string ignored;
        std::int64_t parent = 0;
        stat_stream >> ignored;
        std::string line;
        std::getline(stat_stream, line);
        const std::size_t close = line.rfind(')');
        if (close == std::string::npos) {
            continue;
        }
        std::istringstream fields(line.substr(close + 1));
        std::string state;
        fields >> state >> parent;
        if (managed_process_group >= 0 && parent == managed_process_group) {
            continue;
        }
        return std::int64_t(std::strtoll(name.c_str(), nullptr, 10));
    }
    return std::nullopt;
#endif
}

bool registry_value_exists(const std::wstring& key_path, const std::wstring& name) {
    return registry_read(key_path, name).has_value();
}

std::optional<std::wstring> registry_read(const std::wstring& key_path, const std::wstring& name) {
#if !defined(_WIN32)
    static_cast<void>(key_path);
    static_cast<void>(name);
    return std::nullopt;
#else
    const std::size_t split = key_path.find(L'\\');
    if (split == std::wstring::npos) {
        return std::nullopt;
    }
    HKEY root = nullptr;
    const std::wstring root_name = key_path.substr(0, split);
    if (root_name == L"HKEY_CURRENT_USER") {
        root = HKEY_CURRENT_USER;
    } else if (root_name == L"HKEY_LOCAL_MACHINE") {
        root = HKEY_LOCAL_MACHINE;
    } else {
        return std::nullopt;
    }
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, key_path.substr(split + 1).c_str(), 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ || size == 0) {
        RegCloseKey(key);
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, name.c_str(), nullptr, &type, reinterpret_cast<LPBYTE>(value.data()), &size) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return std::nullopt;
    }
    RegCloseKey(key);
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
#endif
}

void registry_write(const std::wstring& key_path, const std::wstring& name, const std::wstring& value) {
#if !defined(_WIN32)
    static_cast<void>(key_path);
    static_cast<void>(name);
    static_cast<void>(value);
#else
    const std::size_t split = key_path.find(L'\\');
    if (split == std::wstring::npos) {
        return;
    }
    HKEY root = HKEY_CURRENT_USER;
    const std::wstring remainder = key_path.substr(split + 1);
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, remainder.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1U) * sizeof(wchar_t)));
    RegCloseKey(key);
#endif
}

void registry_delete(const std::wstring& key_path, const std::wstring& name) {
#if !defined(_WIN32)
    static_cast<void>(key_path);
    static_cast<void>(name);
#else
    const std::size_t split = key_path.find(L'\\');
    if (split == std::wstring::npos) {
        return;
    }
    HKEY root = HKEY_CURRENT_USER;
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, key_path.substr(split + 1).c_str(), 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
        return;
    }
    RegDeleteValueW(key, name.c_str());
    RegCloseKey(key);
#endif
}

}
