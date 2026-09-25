#include "platform.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>


namespace swapdex {
namespace {

constexpr std::size_t maximum_auth_bytes = 16U * 1024U * 1024U;

int open_parent_directory(const std::filesystem::path& path) {
    return open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

void close_if_open(int& descriptor) {
    if (descriptor >= 0) {
        close(descriptor);
        descriptor = -1;
    }
}

std::filesystem::path parent_directory(const std::filesystem::path& path) {
    return path.parent_path().empty() ? std::filesystem::path(".") : path.parent_path();
}

}

std::optional<std::string> environment_value(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
}

std::filesystem::path default_account_root() {
    return home_directory() / ".swapdex-accounts";
}

std::filesystem::path home_directory() {
    return platform::home_directory();
}

std::filesystem::path state_directory() {
    return platform::state_directory();
}

std::filesystem::path config_directory() {
    return platform::config_directory();
}

std::filesystem::path cache_directory() {
    return platform::cache_directory();
}

std::filesystem::path runtime_directory() {
    return platform::runtime_directory();
}

std::filesystem::path default_codex_home() {
    return platform::default_codex_home();
}

std::filesystem::path default_electron_user_data() {
    return platform::default_electron_user_data();
}

void ensure_directory(const std::filesystem::path& path, std::filesystem::perms permissions) {
    if (path.empty()) {
        throw Error("invalid_path", "Directory path is empty");
    }
    std::error_code error;
    std::filesystem::create_directories(path, error);
    if (error) {
        throw Error("directory_create_failed", "Unable to create a required directory");
    }
    if (!std::filesystem::is_directory(path, error) || error || std::filesystem::is_symlink(path)) {
        throw Error("directory_invalid", "A required path is not a regular directory");
    }
    std::filesystem::permissions(path, permissions, std::filesystem::perm_options::replace, error);
    if (error) {
        throw Error("directory_permissions_failed", "Unable to secure a required directory");
    }
    struct stat status {};
    if (::stat(path.c_str(), &status) != 0 || status.st_uid != getuid()) {
        throw Error("directory_owner_invalid", "A required directory has an unexpected owner");
    }
}

void ensure_private_directory(const std::filesystem::path& path) {
    ensure_directory(path, std::filesystem::perms::owner_all);
}

void write_file_atomically(const std::filesystem::path& path, std::string_view contents, std::filesystem::perms permissions) {
    const auto parent = parent_directory(path);
    ensure_private_directory(parent);
    const auto temporary = parent / ("." + path.filename().string() + "." + random_identifier(8) + ".tmp");
    int descriptor = -1;
    try {
        descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (descriptor < 0) {
            throw Error("file_create_failed", "Unable to create a temporary file");
        }
        if (fchmod(descriptor, static_cast<mode_t>(permissions)) != 0) {
            throw Error("file_permissions_failed", "Unable to secure a temporary file");
        }
        std::size_t offset = 0;
        while (offset < contents.size()) {
            const ssize_t written = write(descriptor, contents.data() + offset, contents.size() - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                throw Error("file_write_failed", "Unable to write a temporary file");
            }
            offset += static_cast<std::size_t>(written);
        }
        if (fsync(descriptor) != 0) {
            throw Error("file_sync_failed", "Unable to synchronize a temporary file");
        }
        close_if_open(descriptor);
        if (rename(temporary.c_str(), path.c_str()) != 0) {
            throw Error("file_replace_failed", "Unable to atomically replace a file");
        }
        int parent_descriptor = open_parent_directory(parent);
        if (parent_descriptor >= 0) {
            fsync(parent_descriptor);
            close_if_open(parent_descriptor);
        }
    } catch (...) {
        close_if_open(descriptor);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

void copy_file_atomically(const std::filesystem::path& source, const std::filesystem::path& destination, std::filesystem::perms permissions) {
    const int source_descriptor = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (source_descriptor < 0) {
        throw Error("source_open_failed", "Unable to open a source file");
    }
    struct stat status {};
    if (fstat(source_descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 || static_cast<std::size_t>(status.st_size) > maximum_auth_bytes) {
        close(source_descriptor);
        throw Error("source_invalid", "The source file is not an acceptable regular file");
    }
    const auto parent = parent_directory(destination);
    ensure_private_directory(parent);
    const auto temporary = parent / ("." + destination.filename().string() + "." + random_identifier(8) + ".tmp");
    int destination_descriptor = -1;
    try {
        destination_descriptor = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (destination_descriptor < 0 || fchmod(destination_descriptor, static_cast<mode_t>(permissions)) != 0) {
            throw Error("copy_create_failed", "Unable to create a secured destination");
        }
        std::array<char, 64U * 1024U> buffer{};
        std::uint64_t total = 0;
        for (;;) {
            const ssize_t count = read(source_descriptor, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                throw Error("copy_read_failed", "Unable to read the source file");
            }
            if (count == 0) {
                break;
            }
            total += static_cast<std::uint64_t>(count);
            if (total > maximum_auth_bytes) {
                throw Error("copy_too_large", "The source file exceeds the permitted size");
            }
            std::size_t offset = 0;
            while (offset < static_cast<std::size_t>(count)) {
                const ssize_t written = write(destination_descriptor, buffer.data() + offset, static_cast<std::size_t>(count) - offset);
                if (written < 0 && errno == EINTR) {
                    continue;
                }
                if (written <= 0) {
                    throw Error("copy_write_failed", "Unable to write the destination file");
                }
                offset += static_cast<std::size_t>(written);
            }
        }
        if (fsync(destination_descriptor) != 0) {
            throw Error("copy_sync_failed", "Unable to synchronize the destination file");
        }
        close_if_open(destination_descriptor);
        close(source_descriptor);
        if (rename(temporary.c_str(), destination.c_str()) != 0) {
            throw Error("copy_replace_failed", "Unable to atomically replace the destination");
        }
        int parent_descriptor = open_parent_directory(parent);
        if (parent_descriptor >= 0) {
            fsync(parent_descriptor);
            close_if_open(parent_descriptor);
        }
    } catch (...) {
        close_if_open(destination_descriptor);
        close(source_descriptor);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

std::string read_file(const std::filesystem::path& path, std::size_t maximum_bytes) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
        throw Error("read_failed", "Unable to open a required file");
    }
    struct stat status {};
    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 || static_cast<std::size_t>(status.st_size) > maximum_bytes) {
        close(descriptor);
        throw Error("read_invalid", "The requested file is invalid or too large");
    }
    const std::size_t file_size = static_cast<std::size_t>(status.st_size);
    std::string contents(file_size, '\0');
    std::size_t offset = 0;
    while (offset < file_size) {
        const ssize_t count = read(descriptor, contents.data() + offset, file_size - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(descriptor);
            throw Error("read_failed", "Unable to read a required file");
        }
        offset += static_cast<std::size_t>(count);
    }
    close(descriptor);
    return contents;
}

Json read_json_file(const std::filesystem::path& path, std::size_t maximum_bytes) {
    return Json::parse(read_file(path, maximum_bytes));
}

void write_json_file_atomically(const std::filesystem::path& path, const Json& value, std::filesystem::perms permissions) {
    write_file_atomically(path, value.dump(2) + "\n", permissions);
}

std::string random_identifier(std::size_t bytes) {
    if (bytes == 0 || bytes > 64) {
        throw Error("identifier_size_invalid", "Identifier size is outside the accepted range");
    }
    const int descriptor = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        throw Error("random_source_failed", "Unable to open the system random source");
    }
    std::string bytes_value(bytes, '\0');
    std::size_t offset = 0;
    while (offset < bytes) {
        const ssize_t count = read(descriptor, bytes_value.data() + offset, bytes_value.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            close(descriptor);
            throw Error("random_source_failed", "Unable to read the system random source");
        }
        offset += static_cast<std::size_t>(count);
    }
    close(descriptor);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const unsigned char byte : bytes_value) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

std::uint64_t monotonic_milliseconds() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

std::string format_timestamp(std::int64_t unix_seconds) {
    const std::time_t value = static_cast<std::time_t>(unix_seconds);
    std::tm utc {};
    if (gmtime_r(&value, &utc) == nullptr) {
        return "unknown";
    }
    std::array<char, 32> buffer {};
    if (std::strftime(buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%SZ", &utc) == 0) {
        return "unknown";
    }
    return buffer.data();
}

std::optional<std::int64_t> json_optional_integer(const Json& value, std::string_view key) {
    if (!value.is_object() || !value.contains(key) || value.at(key).is_null()) {
        return std::nullopt;
    }
    try {
        return value.at(key).get<std::int64_t>();
    } catch (const Json::exception&) {
        return std::nullopt;
    }
}

std::optional<std::string> json_optional_string(const Json& value, std::string_view key) {
    if (!value.is_object() || !value.contains(key) || !value.at(key).is_string()) {
        return std::nullopt;
    }
    auto result = value.at(key).get<std::string>();
    if (result.empty()) {
        return std::nullopt;
    }
    return result;
}

std::string sanitize_label(std::string_view label) {
    std::string result;
    bool pending_space = false;
    for (const unsigned char character : label) {
        if (character < 0x20U || character == 0x7fU) {
            pending_space = !result.empty();
            continue;
        }
        if (character == ' ' || character == '\t') {
            pending_space = !result.empty();
            continue;
        }
        if (pending_space) {
            result.push_back(' ');
            pending_space = false;
        }
        result.push_back(static_cast<char>(character));
        if (result.size() >= 80U) {
            break;
        }
    }
    return result.empty() ? "Account" : result;
}

bool valid_profile_id(std::string_view id) {
    if (id.empty() || id.size() > 64U) {
        return false;
    }
    for (const unsigned char character : id) {
        if (!std::isalnum(character) && character != '-' && character != '_') {
            return false;
        }
    }
    return true;
}

bool running_under_same_process_group(pid_t first, pid_t second) {
    if (first <= 0 || second <= 0) {
        return false;
    }
#if defined(_WIN32)
    static_cast<void>(first);
    static_cast<void>(second);
    return false;
#else
    return getpgid(first) == getpgid(second);
#endif
}

std::optional<pid_t> running_unmanaged_chatgpt(pid_t managed_process_group) {
    return platform::running_unmanaged_chatgpt(managed_process_group);
}

std::vector<std::string> sanitized_environment(const std::vector<std::pair<std::string, std::string>>& overrides) {
    static constexpr std::array<const char*, 23U> allowed = {
        "HOME", "USER", "LOGNAME", "PATH", "DISPLAY", "WAYLAND_DISPLAY", "XAUTHORITY",
        "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS", "LANG", "LC_ALL", "LC_CTYPE",
        "DESKTOP_SESSION", "XDG_SESSION_TYPE", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
        "XDG_DATA_HOME", "XDG_STATE_HOME", "TMPDIR", "GTK_IM_MODULE", "QT_IM_MODULE",
        "XMODIFIERS", "SWAPDEX_MANAGED"
    };
    std::vector<std::pair<std::string, std::string>> values;
    for (const char* name : allowed) {
        if (const char* value = std::getenv(name); value != nullptr && *value != '\0') {
            values.emplace_back(name, value);
        }
    }
    for (const auto& [name, value] : overrides) {
        std::erase_if(values, [&](const auto& item) { return item.first == name; });
        values.emplace_back(name, value);
    }
    std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    std::vector<std::string> result;
    result.reserve(values.size());
    for (const auto& [name, value] : values) {
        if (name.find('=') != std::string::npos || value.find('\0') != std::string::npos) {
            throw Error("environment_invalid", "The process environment contains an invalid entry");
        }
        result.push_back(name + "=" + value);
    }
    return result;
}

std::string executable_directory() {
    return platform::to_native(platform::executable_directory());
}

}
