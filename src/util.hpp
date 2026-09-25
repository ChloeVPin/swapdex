#pragma once

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace swapdex {

using Json = nlohmann::json;

class Error : public std::runtime_error {
public:
    Error(std::string code, std::string message)
        : std::runtime_error(std::move(message)), code_(std::move(code)) {}

    const std::string& code() const noexcept {
        return code_;
    }

private:
    std::string code_;
};

std::optional<std::string> environment_value(const char* name);
std::filesystem::path home_directory();
std::filesystem::path state_directory();
std::filesystem::path config_directory();
std::filesystem::path cache_directory();
std::filesystem::path runtime_directory();
std::filesystem::path default_codex_home();
std::filesystem::path default_electron_user_data();

void ensure_private_directory(const std::filesystem::path& path);
void ensure_directory(const std::filesystem::path& path, std::filesystem::perms permissions);
void write_file_atomically(const std::filesystem::path& path, std::string_view contents, std::filesystem::perms permissions);
void copy_file_atomically(const std::filesystem::path& source, const std::filesystem::path& destination, std::filesystem::perms permissions);
std::string read_file(const std::filesystem::path& path, std::size_t maximum_bytes);
Json read_json_file(const std::filesystem::path& path, std::size_t maximum_bytes);
void write_json_file_atomically(const std::filesystem::path& path, const Json& value, std::filesystem::perms permissions);

std::string random_identifier(std::size_t bytes = 16);
std::uint64_t monotonic_milliseconds();
std::string format_timestamp(std::int64_t unix_seconds);
std::optional<std::int64_t> json_optional_integer(const Json& value, std::string_view key);
std::optional<std::string> json_optional_string(const Json& value, std::string_view key);
std::string sanitize_label(std::string_view label);
// Codex creates a control socket inside CODEX_HOME, and a Unix socket path cannot
// exceed this many bytes.
constexpr std::size_t unix_socket_path_limit = 107U;
std::filesystem::path codex_control_socket_path(const std::filesystem::path& account_home);
bool valid_profile_id(std::string_view id);
bool running_under_same_process_group(pid_t first, pid_t second);
std::optional<pid_t> running_unmanaged_chatgpt(pid_t managed_process_group);
std::vector<std::string> sanitized_environment(const std::vector<std::pair<std::string, std::string>>& overrides);
std::string executable_directory();

}
