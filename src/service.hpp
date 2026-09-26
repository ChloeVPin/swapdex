#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "app_server.hpp"
#include "cdp.hpp"
#include "profile_store.hpp"

namespace swapdex {

// Every app page target, whatever its path. Exposed so the rule can be tested: the
// settings window is a separate target and used to be excluded.
std::vector<std::string> app_page_target_ids(const nlohmann::json& targets);

namespace service_detail {


constexpr bool should_run_maintenance(bool first_cycle, bool request_pending, bool enabled) {
    return request_pending || (first_cycle && enabled);
}

}

struct ServiceOptions {
    std::optional<std::filesystem::path> codex_home;
    std::optional<std::filesystem::path> electron_user_data;
    bool add_profile = false;
    std::string label;
};

class Service {
public:
    explicit Service(ServiceOptions options);
    ~Service();

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;

    int run();
    int list();
    int add(std::string label);

private:
    void request_stop();
    void process_ui_events();
    void handle_ui_payload(const std::string& payload);
    void connect_browser();
    void send_snapshot();
    void refresh_profiles();
    void start_account_maintenance();
    void account_maintenance_loop();
    void maintain_accounts(const std::optional<std::string>& target_id);
    void launch_onboarding(const std::string& id, bool reauthenticate = false);
    // True once an account has credentials on disk, which is what actually ends a sign-in.
    bool credential_present(const std::string& id);
    void switch_profile(const std::string& id);
    void remove_profile(const std::string& id);
    void report_error(std::string message);
    void report_status(std::string message);
    std::string injection_source() const;
    std::vector<std::string> find_targets(const nlohmann::json& targets) const;
    // Attaches to one app window and injects, returning true when the interface verified.
    bool attach_window(const nlohmann::json& target);
    bool primary_session_ready() const;
    std::string primary_session_id() const;
    void clear_sessions();
    void send_snapshot_to(const std::string& session);
    std::vector<std::string> session_ids() const;
    void acquire_singleton_lock();
    void release_singleton_lock();
    void start_signal_thread();
    void close_signal_pipe();
    int signal_pipe_read_ = -1;
    int signal_pipe_write_ = -1;
    void stop_background_threads();

    ServiceOptions options_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> browser_running_{false};
    std::atomic<bool> transitioning_{false};
    std::atomic<bool> maintenance_enabled_{true};
    std::atomic<bool> maintenance_config_received_{false};
    std::atomic<std::uint32_t> maintenance_interval_minutes_{720U};
    std::atomic<std::uint64_t> maintenance_generation_{0U};
    std::atomic<bool> snapshot_dirty_{false};
    std::mutex maintenance_mutex_;
    std::condition_variable maintenance_condition_;
    std::deque<std::optional<std::string>> maintenance_requests_;
    std::mutex app_server_mutex_;
    std::mutex account_operation_mutex_;
    std::thread ui_thread_;
    std::thread maintenance_thread_;
    std::thread signal_thread_;
    std::optional<CdpPipe> cdp_;
    std::optional<ProfileStore> store_;
    std::optional<AppServerClient> app_server_;
    // Every app window gets its own session, because the main window and the settings
    // window are separate targets and only the main one used to be injected.
    mutable std::mutex sessions_mutex_;
    std::vector<std::string> session_ids_;
    std::string primary_session_;
    std::shared_ptr<platform::InstanceLock> singleton_lock_;
};

}
