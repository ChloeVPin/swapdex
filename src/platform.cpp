#include "platform.hpp"

#include "websocket.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <optional>
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
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>
#include <shobjidl.h>
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
#if !defined(_WIN32)
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
#endif

}

namespace {

std::optional<std::string> environment(const char* name) {
#if defined(_WIN32)
    // MSVC marks getenv unsafe; _dupenv_s is the same lookup with owned storage.
    char* buffer = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0) {
        return std::nullopt;
    }
    if (buffer == nullptr || length <= 1U) {
        std::free(buffer);
        return std::nullopt;
    }
    std::string value(buffer);
    std::free(buffer);
    return value;
#else
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

#if defined(_WIN32)

}

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

namespace {

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

// The store install lives under WindowsApps, which is ACL blocked for direct
// execution. The only way in is package activation, which needs the package's
// application user model id, so the id is derived from the install layout rather
// than hardcoded: the manifest gives the package name and application id, and the
// directory name carries the publisher hash.

struct PackageIdentity {
    std::filesystem::path install_root;
    std::wstring aumid;
};

// Reads a quoted attribute out of the package manifest. The file is UTF-8 but the
// names we look for are plain ASCII, so a byte scan is enough.
std::optional<std::string> manifest_attribute(const std::string& xml, const std::string& tag, const std::string& attribute) {
    const std::size_t tag_at = xml.find(tag);
    if (tag_at == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t end = xml.find('>', tag_at);
    const std::string needle = attribute + "=\"";
    const std::size_t attr_at = xml.find(needle, tag_at);
    if (attr_at == std::string::npos || (end != std::string::npos && attr_at > end)) {
        return std::nullopt;
    }
    const std::size_t value_at = attr_at + needle.size();
    const std::size_t close = xml.find('"', value_at);
    if (close == std::string::npos) {
        return std::nullopt;
    }
    return xml.substr(value_at, close - value_at);
}

std::optional<PackageIdentity> package_identity_for(const std::filesystem::path& executable) {
    // Layout: ...\WindowsApps\<Name>_<ver>_<arch>__<hash>\app\ChatGPT.exe
    const std::filesystem::path app_dir = executable.parent_path();
    const std::filesystem::path package_dir = app_dir.parent_path();
    if (app_dir.filename() != "app" || package_dir.parent_path().filename() != "WindowsApps") {
        return std::nullopt;
    }
    const std::wstring dir_name = package_dir.filename().wstring();
    const std::size_t publisher_at = dir_name.rfind(L"__");
    if (publisher_at == std::wstring::npos) {
        return std::nullopt;
    }
    std::ifstream manifest(package_dir / "AppxManifest.xml", std::ios::binary);
    if (!manifest) {
        return std::nullopt;
    }
    const std::string xml((std::istreambuf_iterator<char>(manifest)), std::istreambuf_iterator<char>());
    const auto package_name = manifest_attribute(xml, "<Identity ", "Name");
    const auto application_id = manifest_attribute(xml, "<Application ", "Id");
    if (!package_name.has_value() || !application_id.has_value()) {
        return std::nullopt;
    }
    PackageIdentity identity;
    identity.install_root = package_dir;
    identity.aumid = widen(*package_name) + L"_" + dir_name.substr(publisher_at + 2) + L"!" + widen(*application_id);
    return identity;
}

// One arg to a Windows command line, quoted the way CommandLineToArgvW unquotes it.
std::wstring quote_argument(const std::wstring& argument) {
    if (argument.find_first_of(L" \t\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(character);
        }
        backslashes = 0;
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring join_command_line(const std::filesystem::path& executable, const std::vector<std::string>& arguments) {
    std::wstring line = quote_argument(executable.wstring());
    for (const auto& argument : arguments) {
        line.push_back(L' ');
        line.append(quote_argument(widen(argument)));
    }
    return line;
}

// ------ Other processes ------

struct ObservedProcess {
    std::int64_t pid = 0;
    std::int64_t parent = 0;
    std::wstring executable;
    std::wstring command_line;
};

using NtQueryInformationProcess_t = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

// Only the 64 bit layout matters: the packaged app is x64 and Swapdex only builds
// x64 on Windows.
struct ProcessBasicInformation64 {
    LONG exit_status;
    std::uint64_t peb_base_address;
    std::uint64_t affinity_mask;
    LONG base_priority;
    std::uint64_t unique_process_id;
    std::uint64_t inherited_from_unique_process_id;
};

struct Peb64 {
    std::uint64_t reserved[2];
    std::uint64_t image_base_address;
    std::uint64_t ldr;
    std::uint64_t process_parameters;
};

struct UnicodeString64 {
    std::uint16_t length;
    std::uint16_t maximum_length;
    std::uint32_t padding;
    std::uint64_t buffer;
};

// Field offsets against RTL_USER_PROCESS_PARAMETERS on x64; the fields before
// CommandLine only exist to position it at 0x70, matching the observed layout.
struct UserProcessParameters64 {
    std::uint32_t maximum_length;
    std::uint32_t length;
    std::uint32_t flags;
    std::uint32_t debug_flags;
    std::uint64_t console_handle;
    std::uint32_t console_flags;
    std::uint32_t padding0;
    std::uint64_t standard_input;
    std::uint64_t standard_output;
    std::uint64_t standard_error;
    UnicodeString64 current_directory_path;
    std::uint64_t current_directory_handle;
    UnicodeString64 dll_path;
    UnicodeString64 image_path_name;
    UnicodeString64 command_line;
};

// Another process's command line. NtQueryInformationProcess with
// ProcessCommandLineInformation does it without touching the PEB layout, and the
// PEB read below stays as a fallback on systems where the info class is rejected.
std::optional<std::wstring> process_command_line(DWORD pid) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (process == nullptr) {
        return std::nullopt;
    }
    struct Guard {
        HANDLE handle;
        ~Guard() { CloseHandle(handle); }
    } guard{process};
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return std::nullopt;
    }
    const auto query = reinterpret_cast<NtQueryInformationProcess_t>(GetProcAddress(ntdll, "NtQueryInformationProcess"));
    if (query == nullptr) {
        return std::nullopt;
    }
    constexpr ULONG process_command_line_information = 60;
    ULONG needed = 0;
    query(process, process_command_line_information, nullptr, 0, &needed);
    if (needed > 0 && needed < 1024U * 1024U) {
        std::vector<char> buffer(needed, 0);
        if (query(process, process_command_line_information, buffer.data(), needed, &needed) == 0 && needed >= sizeof(UnicodeString64)) {
            const auto* text = reinterpret_cast<const UnicodeString64*>(buffer.data());
            if (text->length > 0 && static_cast<std::uint64_t>(text->length) <= needed) {
                const wchar_t* first = reinterpret_cast<const wchar_t*>(buffer.data() + sizeof(UnicodeString64));
                return std::wstring(first, first + text->length / sizeof(wchar_t));
            }
        }
    }
    ProcessBasicInformation64 info{};
    ULONG returned = 0;
    if (query(process, 0 /* ProcessBasicInformation */, &info, sizeof(info), &returned) != 0 || info.peb_base_address == 0) {
        return std::nullopt;
    }
    Peb64 peb{};
    SIZE_T read = 0;
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(info.peb_base_address), &peb, sizeof(peb), &read) || peb.process_parameters == 0) {
        return std::nullopt;
    }
    UserProcessParameters64 parameters{};
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(peb.process_parameters), &parameters, sizeof(parameters), &read)) {
        return std::nullopt;
    }
    if (parameters.command_line.buffer == 0 || parameters.command_line.length == 0) {
        return std::nullopt;
    }
    std::wstring result(parameters.command_line.length / sizeof(wchar_t), L'\0');
    if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(parameters.command_line.buffer), result.data(), parameters.command_line.length, &read)) {
        return std::nullopt;
    }
    return result;
}

// Every ChatGPT.exe in the snapshot. The browser process carries no --type= flag;
// its renderer and utility children do, so the filter is just the exe name plus a
// command line check on the caller's side.
std::vector<ObservedProcess> enumerate_chatgpt_processes() {
    std::vector<ObservedProcess> processes;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return processes;
    }
    struct Guard {
        HANDLE handle;
        ~Guard() { CloseHandle(handle); }
    } guard{snapshot};
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        std::wstring lowered = entry.szExeFile;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        if (lowered != L"chatgpt.exe") {
            continue;
        }
        ObservedProcess observed;
        observed.pid = static_cast<std::int64_t>(entry.th32ProcessID);
        observed.parent = static_cast<std::int64_t>(entry.th32ParentProcessID);
        observed.executable = entry.szExeFile;
        if (auto command_line = process_command_line(entry.th32ProcessID); command_line.has_value()) {
            observed.command_line = std::move(*command_line);
        }
        processes.push_back(std::move(observed));
    }
    return processes;
}

bool command_line_has(const std::wstring& command_line, const std::wstring& flag) {
    return command_line.find(flag) != std::wstring::npos;
}

// A "browser" ChatGPT process is the top level instance: no Chromium --type child
// marker and a readable command line.
bool is_browser_process(const ObservedProcess& process) {
    if (process.command_line.empty()) {
        return false;
    }
    return !command_line_has(process.command_line, L"--type=");
}

bool process_alive(DWORD pid) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (process == nullptr) {
        return false;
    }
    const DWORD state = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return state == WAIT_TIMEOUT;
}

// A private file handle suitable for a detached child's output.
HANDLE open_log_handle(const std::filesystem::path& path) {
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    const HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return nullptr;
    }
    SetFilePointer(handle, 0, nullptr, FILE_END);
    return handle;
}

struct StartedProcess {
    PROCESS_INFORMATION info{};
    bool started = false;
};

// Command creation shared by the run and spawn helpers. Null handles inherit the
// current process's streams. bInheritHandles on its own would leak every
// inheritable handle in this process into the child (a long-lived child keeps a
// caller's pipe or console open forever), so the inherited set is restricted to
// exactly the std handles via PROC_THREAD_ATTRIBUTE_HANDLE_LIST.
StartedProcess create_child_process(const std::filesystem::path& executable, const std::vector<std::string>& arguments, HANDLE std_input, HANDLE std_output, HANDLE std_error, DWORD flags) {
    StartedProcess result;
    STARTUPINFOEXW startupex{};
    startupex.StartupInfo.cb = sizeof(startupex);
    std::vector<HANDLE> inherited;
    std::vector<char> attribute_storage;
    if (std_input != nullptr || std_output != nullptr || std_error != nullptr) {
        startupex.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        startupex.StartupInfo.hStdInput = std_input != nullptr ? std_input : GetStdHandle(STD_INPUT_HANDLE);
        startupex.StartupInfo.hStdOutput = std_output != nullptr ? std_output : GetStdHandle(STD_OUTPUT_HANDLE);
        startupex.StartupInfo.hStdError = std_error != nullptr ? std_error : GetStdHandle(STD_ERROR_HANDLE);
        for (HANDLE handle : {startupex.StartupInfo.hStdInput, startupex.StartupInfo.hStdOutput, startupex.StartupInfo.hStdError}) {
            if (handle != nullptr && handle != INVALID_HANDLE_VALUE && std::find(inherited.begin(), inherited.end(), handle) == inherited.end()) {
                inherited.push_back(handle);
            }
        }
        SIZE_T attribute_size = 0;
        if (inherited.empty() || InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size) != FALSE || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            return result;
        }
        attribute_storage.resize(attribute_size);
        startupex.lpAttributeList = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        if (InitializeProcThreadAttributeList(startupex.lpAttributeList, 1, 0, &attribute_size) == FALSE) {
            return result;
        }
        if (UpdateProcThreadAttribute(startupex.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr) == FALSE) {
            DeleteProcThreadAttributeList(startupex.lpAttributeList);
            return result;
        }
        flags |= EXTENDED_STARTUPINFO_PRESENT;
    }
    std::wstring command = join_command_line(executable, arguments);
    if (CreateProcessW(executable.wstring().c_str(), command.data(), nullptr, nullptr, !inherited.empty() ? TRUE : FALSE, flags, nullptr, nullptr, &startupex.StartupInfo, &result.info)) {
        result.started = true;
    }
    if (startupex.lpAttributeList != nullptr) {
        DeleteProcThreadAttributeList(startupex.lpAttributeList);
    }
    return result;
}

// Versions sort inside the package directory name, so the newest install wins.
std::vector<std::filesystem::path> packaged_chatgpt_candidates() {
    std::vector<std::filesystem::path> found;
    const std::filesystem::path windows_apps = std::filesystem::path(windows_environment("ProgramFiles").value_or("C:\\Program Files")) / "WindowsApps";
    std::error_code error;
    if (!std::filesystem::is_directory(windows_apps, error) || error) {
        return found;
    }
    for (const auto& entry : std::filesystem::directory_iterator(windows_apps, error)) {
        if (error) {
            break;
        }
        const std::wstring name = entry.path().filename().wstring();
        if (name.rfind(L"OpenAI.Codex_", 0) != 0) {
            continue;
        }
        const std::filesystem::path executable = entry.path() / "app" / "ChatGPT.exe";
        if (std::filesystem::is_regular_file(executable, error) && !error) {
            found.push_back(executable);
        }
    }
    std::sort(found.begin(), found.end(), [](const std::filesystem::path& a, const std::filesystem::path& b) {
        return a.parent_path().parent_path().filename().wstring() > b.parent_path().parent_path().filename().wstring();
    });
    return found;
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
        "PROGRAMFILES", "PROGRAMFILES(X86)", "ProgramW6432", "PUBLIC", "HOMEDRIVE", "HOMEPATH",
        "NUMBER_OF_PROCESSORS", "PROCESSOR_ARCHITECTURE",
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
    // The packaged app keeps its Electron profile two levels under the Roaming
    // product directory, not at the product directory itself.
    return config_directory() / "Codex" / "web" / "Codex";
#elif defined(__APPLE__)
    return home_directory() / "Library" / "Application Support" / "Codex";
#else
    return config_directory() / "Codex";
#endif
}

std::filesystem::path chatgpt_executable_name() {
#if defined(_WIN32)
    // The packaged GUI binary; app\\Codex.exe is a launcher stub, not the browser.
    return "ChatGPT.exe";
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
    // The packaged install under WindowsApps wins: it is what the app actually is
    // on supported systems. The per user paths stay for hypothetical unpackaged
    // builds.
    for (const auto& packaged : packaged_chatgpt_candidates()) {
        candidates.push_back(packaged);
    }
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

#if defined(_WIN32)
// The bundled CLI still sits inside the ACL blocked package directory, so it
// cannot run in place. It is vendored into the swapdex cache and refreshed
// whenever the app's own binary is newer than the recorded stamp.
std::filesystem::path vendored_cli_copy(const std::filesystem::path& source) {
    const std::filesystem::path vendor_root = cache_directory() / "swapdex" / "vendor";
    std::error_code error;
    std::filesystem::create_directories(vendor_root, error);
    if (error) {
        return {};
    }
    const auto stamp = std::filesystem::last_write_time(source, error);
    if (error) {
        return {};
    }
    const std::filesystem::path target = vendor_root / "codex.exe";
    const std::filesystem::path stamp_file = vendor_root / "codex.stamp";
    bool fresh = std::filesystem::is_regular_file(target, error) && !error;
    if (fresh) {
        std::ifstream existing(stamp_file);
        long long recorded = 0;
        existing >> recorded;
        fresh = existing.good() && recorded == stamp.time_since_epoch().count();
    }
    if (!fresh) {
        std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing, error);
        if (error) {
            return {};
        }
        std::ofstream stamp_out(stamp_file, std::ios::trunc);
        stamp_out << stamp.time_since_epoch().count();
    }
    return target;
}
#endif

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
#if defined(_WIN32)
                    // The bundled CLI still sits inside the ACL blocked package, so it
                    // cannot be executed in place. It is vendored to a writable spot
                    // and the copy is refreshed when the app's own binary changes.
                    return vendored_cli_copy(candidate);
#else
                    return std::filesystem::absolute(candidate).lexically_normal();
#endif
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
    if (arguments.empty()) {
        return 1;
    }
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    const HANDLE null_handle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (null_handle == INVALID_HANDLE_VALUE) {
        return 1;
    }
    const std::vector<std::string> rest(arguments.begin() + 1, arguments.end());
    StartedProcess child = create_child_process(from_native(arguments.front()), rest, null_handle, null_handle, null_handle, CREATE_NO_WINDOW);
    CloseHandle(null_handle);
    if (!child.started) {
        return 1;
    }
    WaitForSingleObject(child.info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(child.info.hProcess, &code);
    CloseHandle(child.info.hThread);
    CloseHandle(child.info.hProcess);
    return static_cast<int>(code);
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
    const std::vector<std::string> rest(arguments.begin() + 1, arguments.end());
    const std::wstring command_line = join_command_line(from_native(arguments.front()), rest);
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
    // CREATE_UNICODE_ENVIRONMENT is required once a custom Unicode environment
    // block is passed; without it CreateProcessW fails with ERROR_INVALID_PARAMETER.
    const DWORD create_flags = environment_block.empty() ? 0U : CREATE_UNICODE_ENVIRONMENT;
    if (CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, FALSE, create_flags, environment_block.empty() ? nullptr : environment_block.data(), nullptr, &startup, &process) == 0) {
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
    return spawn_detached(arguments, {});
}

bool spawn_detached(const std::vector<std::string>& arguments, const std::filesystem::path& log_file) {
    if (arguments.empty()) {
        return false;
    }
#if defined(_WIN32)
    const std::vector<std::string> rest(arguments.begin() + 1, arguments.end());
    HANDLE log_handle = nullptr;
    HANDLE null_handle = nullptr;
    if (!log_file.empty()) {
        log_handle = open_log_handle(log_file);
    }
    if (log_handle == nullptr) {
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = TRUE;
        null_handle = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (null_handle == INVALID_HANDLE_VALUE) {
            null_handle = nullptr;
        }
    }
    const HANDLE output = log_handle != nullptr ? log_handle : null_handle;
    const StartedProcess child = create_child_process(from_native(arguments.front()), rest, output, output, output, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP);
    if (log_handle != nullptr) {
        CloseHandle(log_handle);
    }
    if (null_handle != nullptr) {
        CloseHandle(null_handle);
    }
    if (!child.started) {
        return false;
    }
    CloseHandle(child.info.hThread);
    CloseHandle(child.info.hProcess);
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
    static_cast<void>(log_file);
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
    // The port flag identifies a managed instance, just like --remote-debugging-pipe
    // does on POSIX, because nothing else passes it.
    const auto command_line = process_command_line(static_cast<DWORD>(pid));
    return command_line.has_value() && command_line_has(*command_line, L"--remote-debugging-port");
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

bool make_close_on_exec_pipe(std::intptr_t descriptors[2]) {
    descriptors[0] = -1;
    descriptors[1] = -1;
#if defined(_WIN32)
    // Windows has no close on exec pipes, but a connected loopback socket pair
    // plays the same role for the shutdown channel.
    unsigned short port = 0;
    const std::intptr_t listener = websocket::listen_loopback(port);
    if (listener == websocket::invalid_socket()) {
        return false;
    }
    const std::intptr_t writer = websocket::connect_loopback(port);
    if (writer == websocket::invalid_socket()) {
        websocket::close_socket(listener);
        return false;
    }
    const SOCKET accepted = ::accept(static_cast<SOCKET>(listener), nullptr, nullptr);
    websocket::close_socket(listener);
    if (accepted == INVALID_SOCKET) {
        websocket::close_socket(writer);
        return false;
    }
    u_long non_blocking = 1;
    ::ioctlsocket(accepted, FIONBIO, &non_blocking);
    descriptors[0] = static_cast<std::intptr_t>(accepted);
    descriptors[1] = writer;
    return true;
#elif defined(__linux__)
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
    // An activated app is parented by the system, not by the service, so parentage
    // can only exclude the caller itself and its directly spawned children. Orphaned
    // managed apps are returned on purpose: the caller sorts them from foreign apps
    // with chatgpt_process_is_managed and reclaims them.
    for (const auto& process : enumerate_chatgpt_processes()) {
        if (!is_browser_process(process)) {
            continue;
        }
        if (managed_process_group >= 0 && (process.pid == managed_process_group || process.parent == managed_process_group)) {
            continue;
        }
        return process.pid;
    }
    return std::nullopt;
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

std::int64_t launch_chatgpt(const std::filesystem::path& executable, const std::vector<std::string>& arguments) {
#if defined(_WIN32)
    if (const auto identity = package_identity_for(executable); identity.has_value()) {
        // Packaged apps cannot be spawned directly; the package manager owns their
        // lifetime. Activation is also what the shell itself does.
        if (CoInitializeEx(nullptr, COINIT_MULTITHREADED) == RPC_E_CHANGED_MODE) {
            CoUninitialize();
            if (CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) < 0) {
                return 0;
            }
        }
        IApplicationActivationManager* manager = nullptr;
        const HRESULT created = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
        if (FAILED(created) || manager == nullptr) {
            return 0;
        }
        std::wstring argument_line;
        for (const auto& argument : arguments) {
            if (!argument_line.empty()) {
                argument_line.push_back(L' ');
            }
            argument_line.append(quote_argument(widen(argument)));
        }
        DWORD pid = 0;
        const HRESULT activated = manager->ActivateApplication(identity->aumid.c_str(), argument_line.c_str(), AO_NONE, &pid);
        manager->Release();
        return SUCCEEDED(activated) ? static_cast<std::int64_t>(pid) : 0;
    }
    // Anything else, most importantly the test double, is a plain executable.
    const std::filesystem::path target = executable.parent_path() / executable.filename();
    const StartedProcess child = create_child_process(target, arguments, nullptr, nullptr, nullptr, CREATE_NO_WINDOW);
    if (!child.started) {
        return 0;
    }
    const std::int64_t pid = static_cast<std::int64_t>(child.info.dwProcessId);
    CloseHandle(child.info.hThread);
    CloseHandle(child.info.hProcess);
    return pid;
#else
    static_cast<void>(executable);
    static_cast<void>(arguments);
    return 0;
#endif
}

}
