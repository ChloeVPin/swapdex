#include "cdp.hpp"

#include "platform.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#if !defined(_WIN32)
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <fstream>
#include <vector>

#include "util.hpp"

namespace swapdex {
namespace {

constexpr std::size_t maximum_cdp_frame_bytes = 16U * 1024U * 1024U;

#if defined(_WIN32)
void close_socket(std::intptr_t& socket) {
    if (socket != websocket::invalid_socket()) {
        websocket::close_socket(socket);
        socket = websocket::invalid_socket();
    }
}
#else
void close_descriptor(std::intptr_t& descriptor) {
    if (descriptor >= 0) {
        ::close(static_cast<int>(descriptor));
        descriptor = -1;
    }
}
#endif

}

CdpPipe::CdpPipe(std::filesystem::path executable, std::filesystem::path codex_home, std::filesystem::path electron_user_data)
    : executable_(std::move(executable)),
      codex_home_(std::move(codex_home)),
      electron_user_data_(std::move(electron_user_data)) {
#if !defined(_WIN32)
    std::signal(SIGPIPE, SIG_IGN);
#endif
}

CdpPipe::~CdpPipe() {
    close();
}

#if defined(_WIN32)

void CdpPipe::start() {
    if (running_.load()) {
        return;
    }
    // --remote-debugging-pipe cannot reach a Chromium on Windows, so the channel is
    // a websocket to a loopback port the app publishes in its user data directory.
    const std::filesystem::path port_file = electron_user_data_ / "DevToolsActivePort";
    std::error_code stale_error;
    std::filesystem::remove(port_file, stale_error);
    const std::vector<std::string> arguments = {
        "--remote-debugging-port=0",
        "--user-data-dir=" + electron_user_data_.string(),
    };
    pid_ = platform::launch_chatgpt(executable_, arguments);
    if (pid_ <= 0) {
        throw Error("cdp_spawn_failed", "Unable to start the Codex desktop application");
    }
    process_group_ = pid_;
    // The app announces its debug endpoint by file. First runs can take a while,
    // so the wait is generous but still bounded.
    std::string ws_path;
    unsigned short port = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream published(port_file);
        if (published) {
            std::string line;
            if (std::getline(published, line) && !line.empty()) {
                try {
                    const int parsed = std::stoi(line);
                    if (parsed > 0 && parsed <= 65535) {
                        port = static_cast<unsigned short>(parsed);
                        if (std::getline(published, line)) {
                            ws_path = line;
                        }
                    }
                } catch (const std::exception&) {
                }
            }
            if (port != 0 && !ws_path.empty()) {
                break;
            }
        }
        if (!platform::process_is_alive(pid_)) {
            pid_ = -1;
            process_group_ = -1;
            throw Error("cdp_spawn_failed", "The Codex desktop application exited during startup");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (port == 0 || ws_path.empty()) {
        terminate_child();
        throw Error("cdp_spawn_failed", "The Codex desktop application did not publish its debug endpoint");
    }
    // The port file can appear before the DevTools listener is bound, so retry
    // the connect+handshake until the listener is actually answering.
    std::optional<websocket::WebSocket> socket;
    const auto socket_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < socket_deadline) {
        socket = websocket::WebSocket::connect("127.0.0.1", port, ws_path, std::chrono::seconds(2));
        if (socket.has_value()) {
            break;
        }
        if (!platform::process_is_alive(pid_)) {
            pid_ = -1;
            process_group_ = -1;
            throw Error("cdp_spawn_failed", "The Codex desktop application exited during startup");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!socket.has_value()) {
        terminate_child();
        throw Error("cdp_pipe_failed", "Unable to open the browser control socket");
    }
    socket_.emplace(std::move(*socket));
    running_.store(true);
    reader_ = std::thread([this] { reader_loop(); });
}

#else

void CdpPipe::start() {
    if (running_.load()) {
        return;
    }
    std::intptr_t command_pipe[2] = {-1, -1};
    std::intptr_t response_pipe[2] = {-1, -1};
    if (!platform::make_close_on_exec_pipe(command_pipe) || !platform::make_close_on_exec_pipe(response_pipe)) {
        close_descriptor(command_pipe[0]);
        close_descriptor(command_pipe[1]);
        close_descriptor(response_pipe[0]);
        close_descriptor(response_pipe[1]);
        throw Error("cdp_pipe_failed", "Unable to create private browser communication pipes");
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
#if defined(__APPLE__)
    platform::disclaim_tcc_responsibility(attributes);
#endif
    int null_descriptor = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (null_descriptor < 0) {
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        close_descriptor(command_pipe[0]);
        close_descriptor(command_pipe[1]);
        close_descriptor(response_pipe[0]);
        close_descriptor(response_pipe[1]);
        throw Error("cdp_pipe_failed", "Unable to open the browser null device");
    }
    int child_command_source = fcntl(command_pipe[0], F_DUPFD_CLOEXEC, 10);
    int child_response_source = fcntl(response_pipe[1], F_DUPFD_CLOEXEC, 10);
    if (child_command_source < 0 || child_response_source < 0 || fcntl(child_command_source, F_SETFD, 0) != 0 || fcntl(child_response_source, F_SETFD, 0) != 0) {
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
        close_descriptor(null_descriptor);
        close_descriptor(child_command_source);
        close_descriptor(child_response_source);
        close_descriptor(command_pipe[0]);
        close_descriptor(command_pipe[1]);
        close_descriptor(response_pipe[0]);
        close_descriptor(response_pipe[1]);
        throw Error("cdp_pipe_failed", "Unable to configure child browser descriptors");
    }
    posix_spawn_file_actions_adddup2(&actions, null_descriptor, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, null_descriptor, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, null_descriptor, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, 3);
    posix_spawn_file_actions_addclose(&actions, 4);
    posix_spawn_file_actions_adddup2(&actions, child_command_source, 3);
    posix_spawn_file_actions_adddup2(&actions, child_response_source, 4);
    posix_spawn_file_actions_addclose(&actions, child_command_source);
    posix_spawn_file_actions_addclose(&actions, child_response_source);
    posix_spawn_file_actions_addclose(&actions, null_descriptor);
    const std::vector<std::string> environment_storage = sanitized_environment({
        {"CODEX_HOME", codex_home_.string()},
        {"CODEX_ELECTRON_USER_DATA_PATH", electron_user_data_.string()},
        {"SWAPDEX_MANAGED", "1"},
    });
    std::vector<char*> environment;
    environment.reserve(environment_storage.size() + 1U);
    for (const std::string& item : environment_storage) {
        environment.push_back(const_cast<char*>(item.c_str()));
    }
    environment.push_back(nullptr);
    std::array<std::string, 3> arguments_storage = {
        executable_.string(),
        "--remote-debugging-pipe",
        "--user-data-dir=" + electron_user_data_.string(),
    };
    std::vector<char*> arguments;
    arguments.reserve(arguments_storage.size() + 1U);
    for (std::string& item : arguments_storage) {
        arguments.push_back(item.data());
    }
    arguments.push_back(nullptr);
    pid_t spawned = -1;
    const int result = posix_spawn(&spawned, executable_.c_str(), &actions, &attributes, arguments.data(), environment.data());
    pid_ = spawned;
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close_descriptor(child_command_source);
    close_descriptor(child_response_source);
    close_descriptor(null_descriptor);
    if (result != 0) {
        close_descriptor(command_pipe[0]);
        close_descriptor(command_pipe[1]);
        close_descriptor(response_pipe[0]);
        close_descriptor(response_pipe[1]);
        throw Error("cdp_spawn_failed", "Unable to start the Codex desktop application");
    }
    close_descriptor(command_pipe[0]);
    close_descriptor(response_pipe[1]);
    command_fd_ = command_pipe[1];
    response_fd_ = response_pipe[0];
    if (command_fd_ < 0 || response_fd_ < 0) {
        close();
        throw Error("cdp_pipe_failed", "Unable to retain browser communication pipes");
    }
    const int flags = fcntl(response_fd_, F_GETFL, 0);
    const int command_flags = fcntl(command_fd_, F_GETFL, 0);
    if (flags < 0 || command_flags < 0 || fcntl(response_fd_, F_SETFL, flags | O_NONBLOCK) != 0 || fcntl(command_fd_, F_SETFL, command_flags | O_NONBLOCK) != 0) {
        close();
        throw Error("cdp_pipe_failed", "Unable to configure browser communication pipes");
    }
    process_group_ = pid_;
    running_.store(true);
    reader_ = std::thread([this] { reader_loop(); });
}

#endif

bool CdpPipe::running() const {
    return running_.load();
}

std::int64_t CdpPipe::process_group() const {
    return process_group_;
}

nlohmann::json CdpPipe::request(const std::string& method, const nlohmann::json& params, const std::optional<std::string>& session_id, std::chrono::milliseconds timeout) {
    if (!running_.load()) {
        throw Error("cdp_closed", "The browser control pipe is not running");
    }
    std::uint64_t id = 0;
    auto pending = std::make_shared<PendingRequest>();
    pending->future = std::make_shared<std::future<nlohmann::json>>(pending->promise.get_future());
    {
        std::lock_guard lock(state_mutex_);
        id = next_id_++;
        pending_.emplace(id, pending);
    }
    nlohmann::json message = nlohmann::json::object();
    message["id"] = id;
    message["method"] = method;
    message["params"] = params;
    if (session_id.has_value()) {
        message["sessionId"] = *session_id;
    }
    try {
        write_message(message);
    } catch (...) {
        std::lock_guard lock(state_mutex_);
        pending_.erase(id);
        throw;
    }
    nlohmann::json response;
    if (!wait_for_response(pending, timeout, response)) {
        std::lock_guard lock(state_mutex_);
        pending_.erase(id);
        throw Error("cdp_timeout", "The browser control request exceeded its deadline");
    }
    if (response.contains("error")) {
        // Carry what the browser said, otherwise a rejection is unactionable.
        std::string detail;
        try {
            detail = response.at("error").value("message", std::string());
        } catch (const std::exception&) {
        }
        throw Error("cdp_rpc", "The browser control request was rejected" + (detail.empty() ? "" : ": " + detail));
    }
    if (!response.contains("result")) {
        throw Error("cdp_protocol", "The browser returned an invalid control response");
    }
    const nlohmann::json& result = response.at("result");
    if (result.is_object() && result.contains("exceptionDetails") && !result.at("exceptionDetails").is_null()) {
        // The exception text is the only thing that says what went wrong in the page.
        std::string detail;
        try {
            const nlohmann::json& details = result.at("exceptionDetails");
            detail = details.value("text", std::string());
            const nlohmann::json exception = details.value("exception", nlohmann::json::object());
            const std::string description = exception.value("description", std::string());
            if (!description.empty()) {
                const std::size_t newline = description.find('\n');
                detail += ": " + description.substr(0, newline == std::string::npos ? description.size() : newline);
            }
        } catch (const std::exception&) {
        }
        throw Error("cdp_rpc", "The browser control evaluation failed" + (detail.empty() ? "" : ": " + detail));
    }
    return result;
}

void CdpPipe::set_event_handler(EventHandler handler) {
    std::lock_guard lock(state_mutex_);
    event_handler_ = std::move(handler);
}

void CdpPipe::close() {
#if defined(_WIN32)
    if (!socket_.has_value() && pid_ <= 0) {
        return;
    }
    if (running_.load() && socket_.has_value()) {
        try {
            request("Browser.close", nlohmann::json::object(), std::nullopt, std::chrono::seconds(2));
        } catch (const Error&) {
        }
    }
    running_.store(false);
    if (socket_.has_value()) {
        // Closing the fd under the reader's in-flight recv is the intended
        // wake-up, matching close_descriptor on the POSIX pipe path.
        socket_->close();
    }
#else
    if (command_fd_ < 0 && response_fd_ < 0 && pid_ <= 0) {
        return;
    }
    if (running_.load() && command_fd_ >= 0) {
        try {
            request("Browser.close", nlohmann::json::object(), std::nullopt, std::chrono::seconds(2));
        } catch (const Error&) {
        }
    }
    running_.store(false);
    close_descriptor(command_fd_);
#endif
    terminate_child();
    stop_reader();
#if defined(_WIN32)
    // Only after the reader has joined is it safe to destroy the socket object.
    socket_.reset();
#endif
    process_group_ = -1;
}

void CdpPipe::reader_loop() {
    std::string buffer;
    bool open = true;
    std::string failure_code = "cdp_closed";
    while (open && running_.load()) {
#if defined(_WIN32)
        // socket_ outlives this thread: it is emplaced before the reader starts
        // and only reset after the reader joins, so dereferences here are safe.
        // A close() from another thread shuts the fd down mid-recv, which is
        // the intended unblock.
        if (!socket_.has_value() || !socket_->open()) {
            open = false;
            break;
        }
        const auto frame = socket_->receive(std::chrono::milliseconds(500));
        if (!frame.has_value()) {
            if (!socket_->open()) {
                open = false;
                break;
            }
            continue;
        }
        if (frame->empty() || frame->size() > maximum_cdp_frame_bytes) {
            failure_code = "cdp_protocol";
            open = false;
            break;
        }
        try {
            fulfill(nlohmann::json::parse(*frame));
        } catch (const nlohmann::json::exception&) {
            failure_code = "cdp_protocol";
            open = false;
            break;
        }
        continue;
#else
        pollfd descriptor {static_cast<int>(response_fd_), POLLIN, 0};
        const int ready = poll(&descriptor, 1, 500);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready < 0) {
            open = false;
            break;
        }
        if (ready == 0) {
            continue;
        }
        std::array<char, 64U * 1024U> chunk {};
        const ssize_t count = read(response_fd_, chunk.data(), chunk.size());
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            open = false;
            break;
        }
        if (count == 0) {
            open = false;
            break;
        }
        buffer.append(chunk.data(), static_cast<std::size_t>(count));
        for (;;) {
            const std::size_t delimiter = buffer.find('\0');
            if (delimiter == std::string::npos) {
                if (buffer.size() > maximum_cdp_frame_bytes) {
                    failure_code = "cdp_protocol";
                    open = false;
                }
                break;
            }
            const std::string frame = buffer.substr(0, delimiter);
            buffer.erase(0, delimiter + 1U);
            if (frame.empty()) {
                continue;
            }
            try {
                fulfill(nlohmann::json::parse(frame));
            } catch (const nlohmann::json::exception&) {
                failure_code = "cdp_protocol";
                open = false;
                break;
            }
        }
#endif
    }
    if (!open) {
        running_.store(false);
        std::vector<std::shared_ptr<PendingRequest>> pending;
        EventHandler handler;
        {
            std::lock_guard lock(state_mutex_);
            pending.reserve(pending_.size());
            for (auto& [id, request] : pending_) {
                static_cast<void>(id);
                pending.push_back(request);
            }
            pending_.clear();
            handler = event_handler_;
        }
        for (const auto& request : pending) {
            request->promise.set_exception(std::make_exception_ptr(Error(failure_code, failure_code == "cdp_protocol" ? "The browser returned malformed control data" : "The browser control pipe closed")));
        }
        if (handler) {
            nlohmann::json event = nlohmann::json::object();
            event["method"] = "Swapdex.pipeClosed";
            try {
                handler(event);
            } catch (const std::exception&) {
            }
        }
    }
    std::lock_guard lock(state_mutex_);
    state_condition_.notify_all();
}

void CdpPipe::write_message(const nlohmann::json& message) {
#if defined(_WIN32)
    std::lock_guard lock(write_mutex_);
    if (!socket_.has_value() || !socket_->open() || !socket_->send_text(message.dump())) {
        throw Error("cdp_closed", "The browser control pipe closed while writing");
    }
#else
    const std::string encoded = message.dump() + std::string(1, '\0');
    std::lock_guard lock(write_mutex_);
    std::size_t offset = 0;
    while (offset < encoded.size()) {
        pollfd descriptor {static_cast<int>(command_fd_), POLLOUT, 0};
        const int ready = poll(&descriptor, 1, 5000);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            throw Error("cdp_write_timeout", "The browser control request could not be written");
        }
        const ssize_t written = write(command_fd_, encoded.data() + offset, encoded.size() - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            throw Error("cdp_closed", "The browser control pipe closed while writing");
        }
        offset += static_cast<std::size_t>(written);
    }
#endif
}

bool CdpPipe::wait_for_response(const std::shared_ptr<PendingRequest>& pending, std::chrono::milliseconds timeout, nlohmann::json& response) {
    if (pending->future->wait_for(timeout) != std::future_status::ready) {
        return false;
    }
    response = pending->future->get();
    return true;
}

void CdpPipe::fulfill(const nlohmann::json& message) {
    EventHandler handler;
    if (message.contains("id") && message.at("id").is_number_unsigned()) {
        const std::uint64_t id = message.at("id").get<std::uint64_t>();
        std::shared_ptr<PendingRequest> pending;
        {
            std::lock_guard lock(state_mutex_);
            const auto iterator = pending_.find(id);
            if (iterator != pending_.end()) {
                pending = iterator->second;
                pending_.erase(iterator);
            }
            handler = event_handler_;
        }
        if (pending) {
            pending->promise.set_value(message);
            return;
        }
    } else {
        std::lock_guard lock(state_mutex_);
        handler = event_handler_;
    }
    if (handler) {
        try {
            handler(message);
        } catch (const std::exception&) {
        }
    }
}

void CdpPipe::stop_reader() {
#if defined(_WIN32)
    if (socket_.has_value()) {
        socket_->close();
    }
    if (reader_.joinable()) {
        reader_.join();
    }
#else
    if (reader_.joinable()) {
        close_descriptor(response_fd_);
        reader_.join();
    } else {
        close_descriptor(response_fd_);
    }
#endif
}

void CdpPipe::terminate_child() {
    if (pid_ <= 0) {
        return;
    }
#if defined(_WIN32)
    platform::close_process(pid_);
    pid_ = -1;
    return;
#else
    const pid_t group = pid_;
    const auto leader_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool leader_reaped = false;
    while (std::chrono::steady_clock::now() < leader_deadline) {
        int status = 0;
        const pid_t result = waitpid(pid_, &status, WNOHANG);
        if (result == pid_ || (result < 0 && errno == ECHILD)) {
            leader_reaped = true;
            pid_ = -1;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!leader_reaped) {
        kill(-group, SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
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
        kill(pid_, SIGKILL);
        int status = 0;
        waitpid(pid_, &status, 0);
        pid_ = -1;
    }
    if (kill(-group, 0) != 0 && errno == ESRCH) {
        return;
    }
    kill(-group, SIGTERM);
    const auto group_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < group_deadline) {
        if (kill(-group, 0) != 0 && errno == ESRCH) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    kill(-group, SIGKILL);
#endif
}

}
