#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace swapdex {

class CdpPipe {
public:
    using EventHandler = std::function<void(const nlohmann::json&)>;

    CdpPipe(std::filesystem::path executable, std::filesystem::path codex_home, std::filesystem::path electron_user_data);
    ~CdpPipe();

    CdpPipe(const CdpPipe&) = delete;
    CdpPipe& operator=(const CdpPipe&) = delete;

    void start();
    bool running() const;
    pid_t process_group() const;
    nlohmann::json request(const std::string& method, const nlohmann::json& params = nlohmann::json::object(), const std::optional<std::string>& session_id = std::nullopt, std::chrono::milliseconds timeout = std::chrono::seconds(20));
    void set_event_handler(EventHandler handler);
    void close();

private:
    struct PendingRequest {
        std::promise<nlohmann::json> promise;
        std::shared_ptr<std::future<nlohmann::json>> future;
    };

    void reader_loop();
    void write_message(const nlohmann::json& message);
    bool wait_for_response(const std::shared_ptr<PendingRequest>& pending, std::chrono::milliseconds timeout, nlohmann::json& response);
    void fulfill(const nlohmann::json& message);
    void stop_reader();
    void terminate_child();

    std::filesystem::path executable_;
    std::filesystem::path codex_home_;
    std::filesystem::path electron_user_data_;
    pid_t pid_ = -1;
    pid_t process_group_ = -1;
    int command_fd_ = -1;
    int response_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread reader_;
    mutable std::mutex write_mutex_;
    mutable std::mutex state_mutex_;
    std::condition_variable state_condition_;
    std::unordered_map<std::uint64_t, std::shared_ptr<PendingRequest>> pending_;
    std::uint64_t next_id_ = 1;
    EventHandler event_handler_;
};

}
