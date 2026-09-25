#include "app_server.hpp"

#include "platform.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <vector>

#include "util.hpp"

extern char** environ;

namespace swapdex {
namespace {

constexpr std::size_t maximum_frame_bytes = 4U * 1024U * 1024U;

class AppServerProcess {
public:
    AppServerProcess(const std::filesystem::path& executable, const std::filesystem::path& codex_home) {
        spawn(executable, codex_home);
    }

    ~AppServerProcess() {
        close();
    }

    AppServerProcess(const AppServerProcess&) = delete;
    AppServerProcess& operator=(const AppServerProcess&) = delete;

    Json request(std::uint64_t id, const std::string& method, const Json& params, std::chrono::steady_clock::time_point deadline) {
        Json message = Json::object();
        message["id"] = id;
        message["method"] = method;
        if (!params.is_null()) {
            message["params"] = params;
        }
        const std::string encoded = message.dump() + "\n";
        write_all(encoded, deadline);
        return read_response(id, deadline);
    }

    void notify(const std::string& method) {
        Json message = Json::object();
        message["method"] = method;
        write_all(message.dump() + "\n", std::chrono::steady_clock::now() + std::chrono::seconds(5));
    }

    void close() {
        if (input_fd_ >= 0) {
            ::close(input_fd_);
            input_fd_ = -1;
        }
        if (pid_ <= 0) {
            if (output_fd_ >= 0) {
                ::close(output_fd_);
                output_fd_ = -1;
            }
            return;
        }
        const auto soft_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < soft_deadline) {
            int status = 0;
            const pid_t result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_ || (result < 0 && errno == ECHILD)) {
                pid_ = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        if (pid_ > 0) {
            kill(-pid_, SIGTERM);
            const auto hard_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < hard_deadline) {
                int status = 0;
                const pid_t result = waitpid(pid_, &status, WNOHANG);
                if (result == pid_ || (result < 0 && errno == ECHILD)) {
                    pid_ = -1;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (pid_ > 0) {
            kill(-pid_, SIGKILL);
            int status = 0;
            waitpid(pid_, &status, 0);
            pid_ = -1;
        }
        if (output_fd_ >= 0) {
            ::close(output_fd_);
            output_fd_ = -1;
        }
    }

private:
    void spawn(const std::filesystem::path& executable, const std::filesystem::path& codex_home) {
        int input_pipe[2] = {-1, -1};
        int output_pipe[2] = {-1, -1};
        if (!platform::make_close_on_exec_pipe(input_pipe) || !platform::make_close_on_exec_pipe(output_pipe)) {
            close_if_open(input_pipe[0]);
            close_if_open(input_pipe[1]);
            close_if_open(output_pipe[0]);
            close_if_open(output_pipe[1]);
            throw Error("app_server_pipe_failed", "Unable to create app-server communication pipes");
        }
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawnattr_t attributes;
        posix_spawnattr_init(&attributes);
        sigset_t empty_mask;
        sigemptyset(&empty_mask);
        posix_spawnattr_setsigmask(&attributes, &empty_mask);
        posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);
        posix_spawnattr_setpgroup(&attributes, 0);
        int null_descriptor = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null_descriptor < 0) {
            posix_spawn_file_actions_destroy(&actions);
            posix_spawnattr_destroy(&attributes);
            close_if_open(input_pipe[0]);
            close_if_open(input_pipe[1]);
            close_if_open(output_pipe[0]);
            close_if_open(output_pipe[1]);
            throw Error("app_server_pipe_failed", "Unable to open the app-server null device");
        }
        posix_spawn_file_actions_adddup2(&actions, input_pipe[0], STDIN_FILENO);
        posix_spawn_file_actions_adddup2(&actions, output_pipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, null_descriptor, STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, null_descriptor);
        posix_spawn_file_actions_addclose(&actions, input_pipe[0]);
        posix_spawn_file_actions_addclose(&actions, input_pipe[1]);
        posix_spawn_file_actions_addclose(&actions, output_pipe[0]);
        posix_spawn_file_actions_addclose(&actions, output_pipe[1]);
        const std::vector<std::string> environment_storage = sanitized_environment({
            {"CODEX_HOME", codex_home.string()},
            {"RUST_LOG", "off"},
            {"CODEX_INTERNAL_ORIGINATOR_OVERRIDE", "Swapdex"},
            {"SWAPDEX_MANAGED", "1"},
        });
        std::vector<char*> environment;
        environment.reserve(environment_storage.size() + 1U);
        for (const std::string& item : environment_storage) {
            environment.push_back(const_cast<char*>(item.c_str()));
        }
        environment.push_back(nullptr);
        std::array<std::string, 4> arguments_storage = {executable.string(), "app-server", "--stdio", ""};
        std::vector<char*> arguments;
        arguments.reserve(arguments_storage.size() + 1U);
        for (std::string& item : arguments_storage) {
            arguments.push_back(item.data());
        }
        arguments.back() = nullptr;
        const int result = posix_spawn(&pid_, executable.c_str(), &actions, &attributes, arguments.data(), environment.data());
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        close_if_open(null_descriptor);
        if (result != 0) {
            close_if_open(input_pipe[0]);
            close_if_open(input_pipe[1]);
            close_if_open(output_pipe[0]);
            close_if_open(output_pipe[1]);
            throw Error("app_server_spawn_failed", "Unable to start the bundled Codex app-server");
        }
        close_if_open(input_pipe[0]);
        close_if_open(output_pipe[1]);
        input_fd_ = input_pipe[1];
        output_fd_ = output_pipe[0];
        if (input_fd_ < 0 || output_fd_ < 0) {
            close();
            throw Error("app_server_pipe_failed", "Unable to retain app-server communication pipes");
        }
        const int flags = fcntl(output_fd_, F_GETFL, 0);
        if (flags < 0 || fcntl(output_fd_, F_SETFL, flags | O_NONBLOCK) != 0) {
            close();
            throw Error("app_server_pipe_failed", "Unable to configure app-server communication");
        }
    }

    static void close_if_open(int& descriptor) {
        if (descriptor >= 0) {
            ::close(descriptor);
            descriptor = -1;
        }
    }

    void write_all(std::string_view data, std::chrono::steady_clock::time_point deadline) {
        std::size_t offset = 0;
        while (offset < data.size()) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                throw Error("app_server_timeout", "The app-server request exceeded its deadline");
            }
            pollfd descriptor {input_fd_, POLLOUT, 0};
            const int ready = poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(remaining.count(), 1000)));
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready <= 0) {
                throw Error("app_server_timeout", "The app-server request exceeded its deadline");
            }
            const ssize_t written = write(input_fd_, data.data() + offset, data.size() - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                throw Error("app_server_closed", "The app-server closed its input channel");
            }
            offset += static_cast<std::size_t>(written);
        }
    }

    Json read_response(std::uint64_t id, std::chrono::steady_clock::time_point deadline) {
        for (;;) {
            const auto newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                const std::string frame = buffer_.substr(0, newline);
                buffer_.erase(0, newline + 1U);
                if (frame.empty()) {
                    continue;
                }
                Json message = Json::parse(frame);
                if (message.contains("id") && message.at("id").is_number_unsigned() && message.at("id").get<std::uint64_t>() == id) {
                    return message;
                }
                continue;
            }
            if (buffer_.size() > maximum_frame_bytes) {
                throw Error("app_server_protocol", "The app-server response exceeded the permitted size");
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                throw Error("app_server_timeout", "The app-server request exceeded its deadline");
            }
            pollfd descriptor {output_fd_, POLLIN, 0};
            const int ready = poll(&descriptor, 1, static_cast<int>(std::min<std::int64_t>(remaining.count(), 1000)));
            if (ready < 0 && errno == EINTR) {
                continue;
            }
            if (ready == 0) {
                continue;
            }
            if (ready < 0) {
                throw Error("app_server_read_failed", "Unable to read the app-server response");
            }
            std::array<char, 64U * 1024U> chunk {};
            const ssize_t count = read(output_fd_, chunk.data(), chunk.size());
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                throw Error("app_server_read_failed", "Unable to read the app-server response");
            }
            if (count == 0) {
                throw Error("app_server_closed", "The app-server closed before responding");
            }
            buffer_.append(chunk.data(), static_cast<std::size_t>(count));
        }
    }

    pid_t pid_ = -1;
    int input_fd_ = -1;
    int output_fd_ = -1;
    std::string buffer_;
};

Json require_result(const Json& response) {
    if (response.contains("error")) {
        throw Error("app_server_rpc", "The app-server rejected an account request");
    }
    if (!response.contains("result") || !response.at("result").is_object()) {
        throw Error("app_server_protocol", "The app-server returned an invalid response");
    }
    return response.at("result");
}

Json require_account_result(const Json& response) {
    try {
        return require_result(response);
    } catch (const Error& error) {
        if (error.code() == "app_server_rpc") {
            throw Error("app_server_auth_required", "The selected account needs re-authentication");
        }
        throw;
    }
}

std::optional<UsageWindow> parse_window(const Json& value) {
    if (!value.is_object() || !value.contains("usedPercent") || !value.at("usedPercent").is_number_integer()) {
        return std::nullopt;
    }
    UsageWindow window;
    const std::int64_t used = value.at("usedPercent").get<std::int64_t>();
    window.remaining_percent = static_cast<int>(std::clamp<std::int64_t>(100 - used, 0, 100));
    window.resets_at = json_optional_integer(value, "resetsAt");
    window.duration_minutes = json_optional_integer(value, "windowDurationMins");
    return window;
}

std::string read_account_id(const Json& account_response) {
    if (account_response.contains("workspaceRouting") && account_response.at("workspaceRouting").is_object()) {
        const auto id = json_optional_string(account_response.at("workspaceRouting"), "chatgptAccountId");
        if (id.has_value()) {
            return *id;
        }
    }
    return {};
}

std::vector<ResetCreditDetail> parse_reset_credits(const Json& value) {
    std::vector<ResetCreditDetail> result;
    if (!value.is_array()) {
        return result;
    }
    for (const Json& item : value) {
        if (!item.is_object() || item.value("status", "unknown") != "available") {
            continue;
        }
        ResetCreditDetail detail;
        detail.reset_type = item.value("resetType", "unknown");
        detail.status = item.value("status", "unknown");
        detail.expires_at = json_optional_integer(item, "expiresAt");
        detail.title = json_optional_string(item, "title").value_or("");
        detail.description = json_optional_string(item, "description").value_or("");
        result.push_back(std::move(detail));
    }
    return result;
}

}

AppServerClient::AppServerClient(std::filesystem::path executable)
    : executable_(std::move(executable)) {
    std::signal(SIGPIPE, SIG_IGN);
}

AppServerSnapshot AppServerClient::query(const std::filesystem::path& codex_home, std::chrono::milliseconds timeout, bool proactive_token_refresh) {
    ensure_private_directory(codex_home);
    AppServerProcess process(executable_, codex_home);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    Json initialize_params = Json::object();
    initialize_params["clientInfo"] = {{"name", "swapdex"}, {"title", "Swapdex"}, {"version", SWAPDEX_VERSION}};
    initialize_params["capabilities"] = {{"experimentalApi", true}, {"optOutNotificationMethods", Json::array({"remoteControl/status/changed"})}};
    const Json initialized = require_result(process.request(1, "initialize", initialize_params, deadline));
    if (!initialized.contains("codexHome") || initialized.at("codexHome").get<std::string>() != codex_home.string()) {
        throw Error("app_server_home_mismatch", "The app-server initialized an unexpected account home");
    }
    process.notify("initialized");
    const Json account_params = {{"refreshToken", proactive_token_refresh}};
    const Json account_before = require_account_result(process.request(2, "account/read", account_params, deadline));
    if (!account_before.contains("account") || !account_before.at("account").is_object() || !account_before.at("account").contains("type") || !account_before.at("account").at("type").is_string() || account_before.at("account").at("type").get<std::string>() != "chatgpt") {
        throw Error("app_server_auth_required", "The selected account is not authenticated with ChatGPT");
    }
    const Json& account = account_before.at("account");
    AppServerSnapshot snapshot;
    snapshot.email = json_optional_string(account, "email");
    snapshot.plan = account.value("planType", "unknown");
    snapshot.account_id = read_account_id(account_before);
    const Json rate_response = process.request(3, "account/rateLimits/read", {{"excludeResetCreditDetails", false}, {"supportsLunaReserve", false}}, deadline);
    if (!rate_response.contains("error") && rate_response.contains("result") && rate_response.at("result").is_object()) {
        const Json& result = rate_response.at("result");
        Json limits = result.value("rateLimits", Json::object());
        if (result.contains("rateLimitsByLimitId") && result.at("rateLimitsByLimitId").is_object()) {
            const Json& by_limit = result.at("rateLimitsByLimitId");
            if (by_limit.contains("codex")) {
                limits = by_limit.at("codex");
            }
        }
        if (limits.is_object()) {
            snapshot.primary_usage = parse_window(limits.value("primary", Json()));
            snapshot.secondary_usage = parse_window(limits.value("secondary", Json()));
            if (limits.contains("credits") && limits.at("credits").is_object()) {
                const Json& credits = limits.at("credits");
                snapshot.credits_balance = json_optional_string(credits, "balance");
                snapshot.credits_unlimited = credits.value("unlimited", false);
                snapshot.credits_available = credits.value("hasCredits", false);
            }
        }
        if (result.contains("rateLimitResetCredits") && result.at("rateLimitResetCredits").is_object()) {
            const Json& reset_summary = result.at("rateLimitResetCredits");
            snapshot.available_reset_credits = json_optional_integer(reset_summary, "availableCount");
            if (reset_summary.contains("credits")) {
                snapshot.reset_credits = parse_reset_credits(reset_summary.at("credits"));
            }
        }
        if (snapshot.account_id.empty()) {
            snapshot.account_id = json_optional_string(result, "accountId").value_or("");
        }
    }
    const Json usage_response = process.request(4, "account/usage/read", Json::object(), deadline);
    if (!usage_response.contains("error") && usage_response.contains("result") && usage_response.at("result").is_object()) {
        const Json& result = usage_response.at("result");
        if (result.contains("summary") && result.at("summary").is_object()) {
            snapshot.lifetime_tokens = json_optional_integer(result.at("summary"), "lifetimeTokens");
        }
    }
    const Json account_after = require_account_result(process.request(5, "account/read", account_params, deadline));
    if (!account_after.contains("account") || !account_after.at("account").is_object() || !account_after.at("account").contains("type") || !account_after.at("account").at("type").is_string() || account_after.at("account").at("type").get<std::string>() != "chatgpt") {
        throw Error("app_server_account_changed", "The account changed while usage was being read");
    }
    const std::string account_id_after = read_account_id(account_after);
    const std::optional<std::string> email_after = json_optional_string(account_after.at("account"), "email");
    if (!snapshot.account_id.empty() && !account_id_after.empty() && snapshot.account_id != account_id_after) {
        throw Error("app_server_account_changed", "The account changed while usage was being read");
    }
    if (snapshot.account_id.empty() && snapshot.email.has_value() && email_after.has_value() && *snapshot.email != *email_after) {
        throw Error("app_server_account_changed", "The account changed while usage was being read");
    }
    snapshot.observed_at = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    return snapshot;
}

}
