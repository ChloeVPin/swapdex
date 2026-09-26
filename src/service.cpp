#include "service.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <thread>
#include <vector>

#include <algorithm>
#include <system_error>

#include "app_server.hpp"
#include "cdp.hpp"
#include "profile_store.hpp"
#include "service_control.hpp"
#include "util.hpp"

namespace swapdex {

namespace {

// The service launches the app itself, so the app is a child of this process. Passing
// the real id lets the foreign app check tell our own app apart from one the user
// started by hand, which it could not do when every caller passed a placeholder.
std::int64_t own_process_id() {
    return static_cast<std::int64_t>(::getpid());
}

}

namespace {

constexpr std::size_t maximum_ui_payload_bytes = 4096U;

bool regular_file_exists(const std::filesystem::path& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

Json usage_json(const std::optional<UsageWindow>& usage) {
    if (!usage.has_value() || !usage->remaining_percent.has_value()) {
        return Json();
    }
    return *usage->remaining_percent;
}

void mark_authentication_lost(ProfileStore& store, const ProfileRecord& record) {
    try {
        store.update_metadata(record.id, record.email, record.plan, false);
        store.update_maintenance(record.id, "error");
    } catch (const std::exception&) {
    }
}

struct ResolvedServicePaths {
    std::filesystem::path state_root;
    std::filesystem::path codex_home;
    std::filesystem::path electron_user_data;
};

ResolvedServicePaths resolve_service_paths(const ServiceOptions& options) {
    const auto installed = installed_runtime_paths();
    std::filesystem::path state_root;
    if (const auto xdg_state_home = environment_value("XDG_STATE_HOME"); xdg_state_home.has_value()) {
        state_root = std::filesystem::path(*xdg_state_home) / "swapdex";
    } else if (installed.has_value()) {
        state_root = installed->state_root;
    } else {
        state_root = state_directory() / "swapdex";
    }
    const std::filesystem::path codex_home = options.codex_home.value_or(
        environment_value("CODEX_HOME").value_or(installed.has_value() ? installed->codex_home.string() : default_codex_home().string()));
    const std::filesystem::path electron_user_data = options.electron_user_data.value_or(
        environment_value("CODEX_ELECTRON_USER_DATA_PATH").value_or(installed.has_value() ? installed->electron_user_data.string() : default_electron_user_data().string()));
    return {state_root, codex_home, electron_user_data};
}

}

Service::Service(ServiceOptions options)
    : options_(std::move(options)) {}

Service::~Service() {
    request_stop();
    stop_background_threads();
    if (cdp_.has_value()) {
        cdp_->close();
    }
    browser_running_.store(false);
    release_singleton_lock();
}

int Service::run() {
    stopping_.store(false);
    const ResolvedServicePaths paths = resolve_service_paths(options_);
    const std::filesystem::path& state_root = paths.state_root;
    const std::filesystem::path& codex_home = paths.codex_home;
    const std::filesystem::path& electron_user_data = paths.electron_user_data;
    store_.emplace(state_root, default_account_root(), codex_home, electron_user_data);
    ensure_private_directory(state_root);
    acquire_singleton_lock();
    store_->initialize();
    app_server_.emplace();
    if (const auto pid = running_unmanaged_chatgpt(own_process_id()); pid.has_value()) {
        throw Error("unmanaged_codex_running", "Close the normally launched Codex application before starting Swapdex");
    }
    const auto active = store_->active();
    if (active.has_value()) {
        if (regular_file_exists(codex_home / "auth.json")) {
            store_->capture_live_auth(active->id);
        } else {
            store_->restore_live_auth(active->id);
        }
    } else {
        store_->clear_live_auth();
    }
    start_signal_thread();
    load_remembered_background();
    connect_browser();
    refresh_profiles();
    ui_thread_ = std::thread([this] { process_ui_events(); });
    start_account_maintenance();
    while (!stopping_.load() && (transitioning_.load() || browser_running_.load())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    const bool graceful_shutdown = stopping_.load();
    request_stop();
    stop_background_threads();
    if (cdp_.has_value()) {
        cdp_->close();
    }
    browser_running_.store(false);
    return graceful_shutdown ? 0 : 1;
}

int Service::list() {
    const ResolvedServicePaths paths = resolve_service_paths(options_);
    const std::filesystem::path& state_root = paths.state_root;
    const std::filesystem::path& codex_home = paths.codex_home;
    const std::filesystem::path& electron_user_data = paths.electron_user_data;
    store_.emplace(state_root, default_account_root(), codex_home, electron_user_data);
    const auto active = store_->active_existing();
    for (const ProfileRecord& record : store_->list_existing()) {
        if (!record.authenticated && !record.email.has_value() && record.label == "New account") {
            continue;
        }
        const std::string marker = active.has_value() && active->id == record.id ? "*" : " ";
        std::cout << marker << " " << record.label;
        if (record.email.has_value()) {
            std::cout << " <" << *record.email << ">";
        }
        std::cout << " [" << record.plan << "]";
        if (record.primary_usage.has_value() && record.primary_usage->remaining_percent.has_value()) {
            std::cout << " 5h " << *record.primary_usage->remaining_percent << "%";
        }
        if (record.secondary_usage.has_value() && record.secondary_usage->remaining_percent.has_value()) {
            std::cout << " 7d " << *record.secondary_usage->remaining_percent << "%";
        }
        if (!record.authenticated) {
            std::cout << " sign-in required";
        }
        std::cout << "\n";
    }
    return 0;
}

int Service::add(std::string label) {
    stopping_.store(false);
    const ResolvedServicePaths paths = resolve_service_paths(options_);
    const std::filesystem::path& state_root = paths.state_root;
    const std::filesystem::path& codex_home = paths.codex_home;
    const std::filesystem::path& electron_user_data = paths.electron_user_data;
    store_.emplace(state_root, default_account_root(), codex_home, electron_user_data);
    ensure_private_directory(state_root);
    acquire_singleton_lock();
    store_->initialize();
    app_server_.emplace();
    start_signal_thread();
    const ProfileRecord profile = store_->create_pending(label.empty() ? "New account" : label);
    launch_onboarding(profile.id);
    stop_background_threads();
    const auto updated = store_->find(profile.id);
    if (!updated.has_value() || !updated->authenticated) {
        return 2;
    }
    return 0;
}

void Service::request_stop() {
    stopping_.store(true);
    maintenance_condition_.notify_all();
}

void Service::process_ui_events() {
    while (!stopping_.load()) {
        std::optional<std::string> payload;
        if (cdp_.has_value() && cdp_->running()) {
            for (const std::string& session : session_ids()) {
                try {
                    const nlohmann::json result = cdp_->request("Runtime.evaluate", {{"expression", "window.__swapdexTakePendingRequest && window.__swapdexTakePendingRequest()"}, {"returnByValue", true}}, session, std::chrono::seconds(1));
                    if (result.contains("result") && result.at("result").is_object() && result.at("result").contains("value") && result.at("result").at("value").is_string()) {
                        std::string candidate = result.at("result").at("value").get<std::string>();
                        if (!candidate.empty() && candidate.size() <= maximum_ui_payload_bytes) {
                            payload = std::move(candidate);
                            break;
                        }
                    }
                } catch (const Error& error) {
                    if (error.code() == "cdp_closed" || stopping_.load()) {
                        return;
                    }
                } catch (const std::exception&) {
                }
            }
        }
        if (payload.has_value()) {
            try {
                handle_ui_payload(*payload);
            } catch (const Error&) {
                report_error("The account action could not be completed");
            } catch (const std::exception&) {
                report_error("The account action could not be completed");
            }
        }
        if (snapshot_dirty_.exchange(false)) {
            try {
                send_snapshot();
            } catch (const Error&) {
            } catch (const std::exception&) {
            }
        }
        if (!payload.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

std::filesystem::path chatgpt_binary() {
    const std::filesystem::path discovered = platform::find_chatgpt_binary();
    if (discovered.empty()) {
        throw Error("codex_not_installed", "Swapdex could not find your Codex installation. Install Codex, then start Swapdex again.");
    }
    return discovered;
}

void Service::handle_ui_payload(const std::string& payload) {
    if (payload.empty() || payload.size() > maximum_ui_payload_bytes) {
        return;
    }
    nlohmann::json request;
    try {
        request = nlohmann::json::parse(payload);
    } catch (const nlohmann::json::exception&) {
        return;
    }
    if (!request.is_object() || !request.contains("v") || !request.at("v").is_number_integer() || request.at("v").get<int>() != 1 || !request.contains("source") || !request.at("source").is_string() || request.at("source").get<std::string>() != "profile-dropdown" || !request.contains("action") || !request.at("action").is_string()) {
        return;
    }
    const std::string action = request.at("action").get<std::string>();
    if (action == "snapshot") {
        send_snapshot();
        refresh_profiles();
        return;
    }
    if (action == "reauth") {
        if (!request.contains("key") || !request.at("key").is_string() || !store_.has_value()) {
            return;
        }
        const std::string id = request.at("key").get<std::string>();
        const auto target = store_->find(id);
        if (!target.has_value()) {
            report_error("Select an account that has completed sign-in");
            return;
        }
        launch_onboarding(id, true);
        return;
    }
    if (action == "renderer-error") {
        const std::string scope = request.value("scope", std::string("renderer"));
        const std::string text = request.value("text", std::string("unknown failure"));
        report_error("the interface hit an error in " + scope + ": " + text);
        return;
    }
    if (action == "chat-background-status") {
        // Whether the background actually painted is the question a user asks, and it
        // is invisible from here, so the interface reports what it managed to do.
        const std::string applied = request.value("applied", std::string("no"));
        const std::string surfaces = request.value("surfaces", std::string("?"));
        report_status("Chat background " + applied + ", chat surfaces marked: " + surfaces);
        return;
    }
    if (action == "chat-background-list") {
        send_background_candidates();
        return;
    }
    if (action == "chat-background") {
        set_chat_background(request);
        return;
    }
    if (action == "maintenance-config") {
        if (!request.contains("enabled") || !request.at("enabled").is_boolean() || !request.contains("interval_hours") || !request.at("interval_hours").is_number_integer()) {
            return;
        }
        const int interval_hours = request.at("interval_hours").get<int>();
        if (interval_hours != 5 && interval_hours != 12 && interval_hours != 24) {
            return;
        }
        maintenance_enabled_.store(request.at("enabled").get<bool>());
        maintenance_interval_minutes_.store(static_cast<std::uint32_t>(interval_hours * 60));
        maintenance_config_received_.store(true);
        maintenance_generation_.fetch_add(1U);
        maintenance_condition_.notify_all();
        return;
    }
    if (action == "maintenance-run-now") {
        std::optional<std::string> target_id;
        if (request.contains("key") && !request.at("key").is_null()) {
            if (!request.at("key").is_string() || !store_.has_value()) {
                return;
            }
            target_id = request.at("key").get<std::string>();
            if (target_id->empty()) {
                target_id.reset();
            } else {
                const auto target = store_->find(*target_id);
                if (!target.has_value() || !target->authenticated) {
                    report_error("Select an account that has completed sign-in");
                    return;
                }
            }
        }
        {
            const std::lock_guard<std::mutex> lock(maintenance_mutex_);
            const bool queued = target_id.has_value()
                ? std::any_of(maintenance_requests_.begin(), maintenance_requests_.end(), [&target_id](const auto& queued_id) { return queued_id == target_id; })
                : std::find(maintenance_requests_.begin(), maintenance_requests_.end(), std::nullopt) != maintenance_requests_.end();
            if (!queued) {
                if (maintenance_requests_.size() >= 16U) {
                    maintenance_requests_.pop_front();
                }
                maintenance_requests_.push_back(target_id);
            }
        }
        maintenance_condition_.notify_all();
        return;
    }
    if (action == "switch") {
        if (!request.contains("key") || !request.at("key").is_string()) {
            return;
        }
        const std::string id = request.at("key").get<std::string>();
        const auto target = store_->find(id);
        if (!target.has_value() || !target->authenticated) {
            report_error("Select an account that has completed sign-in");
            return;
        }
        const auto active = store_->active();
        if (active.has_value() && active->id == id) {
            return;
        }
        switch_profile(id);
        return;
    }
    if (action == "remove") {
        if (!request.contains("key") || !request.at("key").is_string() || !store_.has_value()) {
            return;
        }
        const std::string id = request.at("key").get<std::string>();
        if (!store_->find(id).has_value()) {
            report_error("That account is no longer stored");
            send_snapshot();
            return;
        }
        remove_profile(id);
        return;
    }
    if (action == "add") {
        report_status("Opening a new account sign-in…");
        const ProfileRecord profile = store_->create_pending("New account");
        launch_onboarding(profile.id);
    }
}

bool Service::attach_window(const nlohmann::json& target) {
    if (!cdp_.has_value() || !cdp_->running() || !target.is_object() || !target.contains("targetId") || !target.at("targetId").is_string()) {
        return false;
    }
    const std::string target_id = target.at("targetId").get<std::string>();
    try {
        const nlohmann::json attached = cdp_->request("Target.attachToTarget", {{"targetId", target_id}, {"flatten", true}}, std::nullopt, std::chrono::seconds(5));
        if (!attached.contains("sessionId") || !attached.at("sessionId").is_string()) {
            return false;
        }
        const std::string session = attached.at("sessionId").get<std::string>();
        cdp_->request("Runtime.enable", nlohmann::json::object(), session, std::chrono::seconds(5));
        cdp_->request("Page.enable", nlohmann::json::object(), session, std::chrono::seconds(5));
        const std::string source = injection_source();
        // Both forms are needed: the evaluate covers a window that already exists, and
        // the new document hook covers a window that reloads or is created later.
        cdp_->request("Page.addScriptToEvaluateOnNewDocument", {{"source", source}}, session, std::chrono::seconds(10));
        cdp_->request("Runtime.evaluate", {{"expression", source}, {"returnByValue", true}, {"awaitPromise", true}}, session, std::chrono::seconds(10));
        const nlohmann::json verification = cdp_->request("Runtime.evaluate", {{"expression", "window.__swapdexInstalled === true && typeof window.__swapdexApplySnapshot === 'function' && typeof window.__swapdexTakePendingRequest === 'function'"}, {"returnByValue", true}}, session, std::chrono::seconds(5));
        const bool ok = verification.contains("result") && verification.at("result").is_object() && verification.at("result").contains("value")
            && verification.at("result").at("value").is_boolean() && verification.at("result").at("value").get<bool>();
        if (!ok) {
            // Say why, because a silent failure here looks exactly like a healthy
            // service that simply never attaches.
            std::fputs(("swapdex: the interface did not load in a Codex window: " + std::string(injection_source().empty() ? "no asset" : "asset present but rejected") + "\n").c_str(), stderr);
            std::fflush(stderr);
            return false;
        }
        const std::lock_guard<std::mutex> lock(sessions_mutex_);
        if (std::find(session_ids_.begin(), session_ids_.end(), session) == session_ids_.end()) {
            session_ids_.push_back(session);
        }
        if (primary_session_.empty() || target.value("url", std::string()).find("index.html") != std::string::npos) {
            primary_session_ = session;
        }
        return true;
    } catch (const std::exception& error) {
        std::fputs(("swapdex: a Codex window could not be attached: " + std::string(error.what()) + "\n").c_str(), stderr);
        std::fflush(stderr);
        return false;
    }
}

bool Service::primary_session_ready() const {
    const std::lock_guard<std::mutex> lock(sessions_mutex_);
    return !primary_session_.empty();
}

std::string Service::primary_session_id() const {
    const std::lock_guard<std::mutex> lock(sessions_mutex_);
    return primary_session_;
}

void Service::clear_sessions() {
    const std::lock_guard<std::mutex> lock(sessions_mutex_);
    session_ids_.clear();
    primary_session_.clear();
}

std::vector<std::string> Service::session_ids() const {
    const std::lock_guard<std::mutex> lock(sessions_mutex_);
    return session_ids_;
}

void Service::connect_browser() {
    if (!store_.has_value() || !app_server_.has_value()) {
        throw Error("service_not_initialized", "Swapdex is not initialized");
    }
    if (const auto pid = running_unmanaged_chatgpt(own_process_id()); pid.has_value()) {
        throw Error("unmanaged_codex_running", "Close the normally launched Codex application before switching");
    }
    ensure_private_directory(store_->electron_user_data());
    cdp_.emplace(chatgpt_binary(), store_->shared_codex_home(), store_->electron_user_data());
    cdp_->set_event_handler([this](const nlohmann::json& event) {
        if (!event.is_object() || !event.contains("method") || !event.at("method").is_string()) {
            return;
        }
        const std::string method = event.at("method").get<std::string>();
        if (method == "Swapdex.pipeClosed" && !transitioning_.load()) {
            browser_running_.store(false);
            return;
        }
        // The settings window is a separate window that only exists once the user opens
        // it, so it has to be injected when it appears rather than only at startup.
        if ((method == "Target.targetCreated" || method == "Target.targetInfoChanged") && event.contains("params")) {
            attach_window(event.at("params"));
        }
    });
    try {
        cdp_->start();
        cdp_->request("Target.setDiscoverTargets", {{"discover", true}}, std::nullopt, std::chrono::seconds(5));
        const std::string source = injection_source();
        bool injected_any = false;
        std::string first_failure;
        const auto attach_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!stopping_.load() && std::chrono::steady_clock::now() < attach_deadline) {
            const nlohmann::json targets = cdp_->request("Target.getTargets", nlohmann::json::object(), std::nullopt, std::chrono::seconds(2));
            for (const std::string& target_id : find_targets(targets)) {
                if (attach_window(nlohmann::json{{"targetId", target_id}})) {
                    injected_any = true;
                } else if (first_failure.empty()) {
                    first_failure = "the Codex window could not be prepared";
                }
            }
            if (injected_any) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!injected_any) {
            if (!first_failure.empty()) {
                throw Error("cdp_target_missing", first_failure);
            }
            throw Error("cdp_target_missing", "The Codex window could not be identified");
        }
        if (!primary_session_ready()) {
            throw Error("cdp_injection_failed", "The Codex profile menu integration could not be verified");
        }
        transitioning_.store(false);
        browser_running_.store(true);
        send_snapshot();
    } catch (const Error&) {
        if (cdp_.has_value()) {
            cdp_->close();
            cdp_.reset();
        }
        clear_sessions();
        transitioning_.store(false);
        browser_running_.store(false);
        throw;
    } catch (const std::exception&) {
        if (cdp_.has_value()) {
            cdp_->close();
            cdp_.reset();
        }
        clear_sessions();
        transitioning_.store(false);
        browser_running_.store(false);
        throw Error("cdp_protocol", "The browser control exchange was invalid");
    }
}

void Service::send_snapshot() {
    if (!cdp_.has_value() || !cdp_->running() || !store_.has_value()) {
        return;
    }
    for (const std::string& session : session_ids()) {
        send_snapshot_to(session);
    }
}

void Service::send_snapshot_to(const std::string& session) {
    if (session.empty() || !store_.has_value()) {
        return;
    }
    const auto active = store_->active();
    nlohmann::json payload = nlohmann::json::object();
    payload["v"] = 1;
    payload["active"] = active.has_value() ? active->id : "";
    std::string background_data;
    std::string background_path;
    if (chat_background(background_data, background_path)) {
        payload["chatBackground"] = background_data;
        payload["chatBackgroundSource"] = background_path;
    }
    {
        const std::lock_guard<std::mutex> lock(background_mutex_);
        if (!background_candidates_.empty()) {
            payload["chatBackgroundCandidates"] = background_candidates_;
        }
    }
    payload["profiles"] = nlohmann::json::array();
    for (const ProfileRecord& record : store_->list()) {
        nlohmann::json item = nlohmann::json::object();
        item["id"] = record.id;
        item["label"] = record.label;
        item["email"] = record.email.has_value() ? nlohmann::json(*record.email) : nlohmann::json();
        item["plan"] = record.plan;
        item["authenticated"] = record.authenticated;
        item["primary_remaining"] = usage_json(record.primary_usage);
        item["secondary_remaining"] = usage_json(record.secondary_usage);
        item["lifetime_tokens"] = record.lifetime_tokens.has_value() ? nlohmann::json(*record.lifetime_tokens) : nlohmann::json();
        item["credits_balance"] = record.credits_balance.has_value() ? nlohmann::json(*record.credits_balance) : nlohmann::json();
        item["credits_unlimited"] = record.credits_unlimited;
        item["credits_available"] = record.credits_available;
        item["available_reset_credits"] = record.available_reset_credits.has_value() ? nlohmann::json(*record.available_reset_credits) : nlohmann::json();
        item["available_credits"] = item["available_reset_credits"];
        item["reset_credits"] = nlohmann::json::array();
        for (const ResetCreditDetail& detail : record.reset_credits) {
            nlohmann::json reset = nlohmann::json::object();
            reset["reset_type"] = detail.reset_type;
            reset["status"] = detail.status;
            reset["expires_at"] = detail.expires_at.has_value() ? nlohmann::json(*detail.expires_at) : nlohmann::json();
            reset["title"] = detail.title;
            reset["description"] = detail.description;
            item["reset_credits"].push_back(std::move(reset));
        }
        item["last_maintenance_at"] = record.last_maintenance_at.has_value() ? nlohmann::json(*record.last_maintenance_at) : nlohmann::json();
        item["maintenance_status"] = record.maintenance_status;
        payload["profiles"].push_back(std::move(item));
    }
    const std::string expression = "window.__swapdexApplySnapshot && window.__swapdexApplySnapshot(" + payload.dump() + ")";
    if (const std::string primary = primary_session_id(); !primary.empty()) {
            cdp_->request("Runtime.evaluate", {{"expression", expression}, {"returnByValue", true}}, primary, std::chrono::seconds(5));
        }
}

void Service::refresh_profiles() {
    if (!app_server_.has_value() || !store_.has_value()) {
        return;
    }
    const auto active = store_->active();
    const auto refresh_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
    for (const ProfileRecord& record : store_->list()) {
        if (stopping_.load() || !record.authenticated || !regular_file_exists(store_->profile_auth(record.id))) {
            continue;
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(refresh_deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            break;
        }
        try {
            const std::filesystem::path query_home = active.has_value() && active->id == record.id ? store_->shared_codex_home() : store_->profile_home(record.id);
            const AppServerSnapshot snapshot = [&] {
                const std::lock_guard<std::mutex> lock(app_server_mutex_);
                return app_server_->query(query_home, std::min(remaining, std::chrono::milliseconds(20000)));
            }();
            store_->update_metadata(record.id, snapshot.email, snapshot.plan, true);
            store_->update_usage(record.id, snapshot.primary_usage, snapshot.secondary_usage, snapshot.lifetime_tokens, snapshot.credits_balance, snapshot.credits_unlimited, snapshot.credits_available, snapshot.available_reset_credits, snapshot.reset_credits);
            send_snapshot();
        } catch (const Error& error) {
            if (error.code() == "app_server_auth_required") {
                mark_authentication_lost(*store_, record);
                send_snapshot();
            }
        } catch (const std::exception&) {
        }
    }
}

void Service::start_account_maintenance() {
    maintenance_thread_ = std::thread([this] { account_maintenance_loop(); });
}

void Service::account_maintenance_loop() {
    std::uint64_t observed_generation = maintenance_generation_.load();
    bool first_cycle = true;
    while (!stopping_.load()) {
        bool should_run = false;
        bool request_pending = false;
        std::optional<std::string> target_id;
        {
            std::unique_lock lock(maintenance_mutex_);
            if (first_cycle) {
                maintenance_condition_.wait(lock, [this, observed_generation] {
                    return stopping_.load() || maintenance_config_received_.load() || maintenance_generation_.load() != observed_generation || !maintenance_requests_.empty();
                });
            } else if (maintenance_enabled_.load() && maintenance_requests_.empty()) {
                maintenance_condition_.wait_for(lock, std::chrono::minutes(maintenance_interval_minutes_.load()), [this, observed_generation] {
                    return stopping_.load() || maintenance_generation_.load() != observed_generation || !maintenance_requests_.empty();
                });
            } else if (!maintenance_enabled_.load() && maintenance_requests_.empty()) {
                maintenance_condition_.wait(lock, [this, observed_generation] {
                    return stopping_.load() || maintenance_generation_.load() != observed_generation || !maintenance_requests_.empty();
                });
            }
            if (stopping_.load()) {
                break;
            }
            observed_generation = maintenance_generation_.load();
            if (!maintenance_requests_.empty()) {
                request_pending = true;
                target_id = maintenance_requests_.front();
                maintenance_requests_.pop_front();
            }
            should_run = service_detail::should_run_maintenance(first_cycle, request_pending, maintenance_enabled_.load());
            first_cycle = false;
        }
        if (should_run) {
            maintain_accounts(target_id);
        }
    }
}

void Service::maintain_accounts(const std::optional<std::string>& target_id) {
    if (!app_server_.has_value() || !store_.has_value()) {
        return;
    }
    const std::lock_guard<std::mutex> operation_lock(account_operation_mutex_);
    if (stopping_.load()) {
        return;
    }
    std::vector<ProfileRecord> records = store_->list();
    if (target_id.has_value()) {
        const auto target = store_->find(*target_id);
        if (!target.has_value() || !target->authenticated) {
            return;
        }
        records = {*target};
    }
    const auto active = store_->active();
    for (const ProfileRecord& record : records) {
        if (stopping_.load() || !record.authenticated) {
            continue;
        }
        try {
            const std::filesystem::path query_home = active.has_value() && active->id == record.id ? store_->shared_codex_home() : store_->profile_home(record.id);
            if (!regular_file_exists(store_->profile_auth(record.id))) {
                throw Error("missing_auth", "Stored account credentials are missing");
            }
            AppServerSnapshot snapshot;
            {
                const std::lock_guard<std::mutex> lock(app_server_mutex_);
                snapshot = app_server_->query(query_home, std::chrono::seconds(30), true);
                if (active.has_value() && active->id == record.id) {
                    store_->capture_live_auth(record.id);
                }
            }
            store_->update_metadata(record.id, snapshot.email, snapshot.plan, true);
            store_->update_usage(record.id, snapshot.primary_usage, snapshot.secondary_usage, snapshot.lifetime_tokens, snapshot.credits_balance, snapshot.credits_unlimited, snapshot.credits_available, snapshot.available_reset_credits, snapshot.reset_credits);
            store_->update_maintenance(record.id, "ok");
        } catch (const Error& error) {
            if (error.code() == "app_server_auth_required") {
                mark_authentication_lost(*store_, record);
            } else {
                try {
                    store_->update_maintenance(record.id, "error");
                } catch (const std::exception&) {
                }
            }
        } catch (const std::exception&) {
            try {
                store_->update_maintenance(record.id, "error");
            } catch (const std::exception&) {
            }
        }
    }
    snapshot_dirty_.store(true);
}

void Service::launch_onboarding(const std::string& id, bool reauthenticate) {
    if (!store_.has_value() || !app_server_.has_value()) {
        return;
    }
    const std::lock_guard<std::mutex> operation_lock(account_operation_mutex_);
    const bool had_shared_app = cdp_.has_value() && cdp_->running();
    const auto previous = store_->active();
    if (had_shared_app) {
        report_status(reauthenticate ? "Re-authenticating account…" : "Opening a new account sign-in…");
        transitioning_.store(true);
        browser_running_.store(false);
        cdp_->close();
        cdp_.reset();
        clear_sessions();
    }
    if (const auto pid = running_unmanaged_chatgpt(own_process_id()); pid.has_value()) {
        if (had_shared_app && previous.has_value()) {
            try {
                store_->capture_live_auth(previous->id);
                connect_browser();
            } catch (const std::exception&) {
                request_stop();
            }
        }
        store_->prune_pending_placeholders();
        report_error("Close other Codex applications before adding an account");
        return;
    }
    ensure_private_directory(store_->onboarding_user_data(id));
    CdpPipe onboarding(chatgpt_binary(), store_->profile_home(id), store_->onboarding_user_data(id));
    try {
        onboarding.start();
    } catch (const std::exception&) {
        if (had_shared_app && previous.has_value()) {
            try {
                store_->capture_live_auth(previous->id);
                connect_browser();
            } catch (const std::exception&) {
                request_stop();
            }
        }
        store_->prune_pending_placeholders();
        report_error("The account sign-in window could not be opened");
        return;
    }
    report_status(reauthenticate ? "Finish re-authenticating the account, then close its Codex window" : "Sign in to the new account, then close its Codex window");
    // Completion is the credential appearing, not the sign-in window closing. On macOS
    // the app process stays alive after its window is dismissed, so waiting for the
    // process to exit hung here indefinitely and the account could never be added. The
    // wait is bounded either way so it can never hang.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
    bool signed_in = false;
    while (!stopping_.load() && std::chrono::steady_clock::now() < deadline) {
        if (credential_present(id)) {
            signed_in = true;
            break;
        }
        if (!onboarding.running()) {
            // The window is gone. Give the credential a moment to land before giving up.
            for (int grace = 0; grace < 20 && !stopping_.load(); ++grace) {
                if (credential_present(id)) {
                    signed_in = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    onboarding.close();
    if (!signed_in && !stopping_.load() && !credential_present(id)) {
        store_->prune_pending_placeholders();
        report_error("Sign-in was not completed before the window closed");
        send_snapshot();
        return;
    }
    if (stopping_.load()) {
        return;
    }
    try {
        const AppServerSnapshot snapshot = [&] {
            const std::lock_guard<std::mutex> lock(app_server_mutex_);
            return app_server_->query(store_->profile_home(id));
        }();
        store_->update_metadata(id, snapshot.email, snapshot.plan, true);
        store_->update_usage(id, snapshot.primary_usage, snapshot.secondary_usage, snapshot.lifetime_tokens, snapshot.credits_balance, snapshot.credits_unlimited, snapshot.credits_available, snapshot.available_reset_credits, snapshot.reset_credits);
        if (had_shared_app) {
            store_->activate(id);
            connect_browser();
        }
        report_status(reauthenticate ? "Account re-authenticated" : "Account added");
    } catch (const std::exception&) {
        if (had_shared_app) {
            try {
                if (previous.has_value()) {
                    store_->restore_live_auth(previous->id);
                    store_->activate(previous->id);
                }
                connect_browser();
            } catch (const std::exception&) {
                request_stop();
            }
        }
        store_->prune_pending_placeholders();
        report_error(reauthenticate ? "The account could not be re-authenticated" : "The new profile was created but sign-in was not completed");
        send_snapshot();
    }
}

namespace service_detail {

// Images are turned into a data URL by the service, because the interface runs inside
// the app and cannot read the user's files. Only formats a browser can paint are
// accepted, and the size is capped so a huge image cannot exhaust memory.
constexpr std::size_t maximum_background_bytes = 4U * 1024U * 1024U;

std::optional<std::string> background_mime_type(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    for (char& character : extension) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".jpg" || extension == ".jpeg") {
        return "image/jpeg";
    }
    if (extension == ".webp") {
        return "image/webp";
    }
    if (extension == ".gif") {
        return "image/gif";
    }
    return std::nullopt;
}

}

void Service::set_chat_background(const nlohmann::json& request) {
    if (!request.contains("path")) {
        clear_chat_background();
        return;
    }
    if (!request.at("path").is_string()) {
        report_error("That is not a usable image path");
        return;
    }
    const std::string raw = request.at("path").get<std::string>();
    if (raw.empty()) {
        clear_chat_background();
        return;
    }
    const std::filesystem::path path(raw);
    std::error_code error;
    if (!path.is_absolute()) {
        report_error("Use the full path to the image, starting with /");
        return;
    }
    const std::optional<std::string> mime = service_detail::background_mime_type(path);
    if (!mime.has_value()) {
        report_error("That image type is not supported. Use a PNG, JPEG, WebP or GIF.");
        return;
    }
    // A symlinked wallpaper is perfectly normal, so resolve rather than refuse. Nothing
    // here can execute anything: the bytes are re-encoded as a data URL and the only
    // formats accepted are raster types.
    const std::filesystem::path resolved = std::filesystem::weakly_canonical(path, error);
    const std::filesystem::path& target = error || resolved.empty() ? path : resolved;
    if (!std::filesystem::exists(target)) {
        report_error("No file exists at " + path.string());
        return;
    }
    if (std::filesystem::is_directory(target, error) || error) {
        report_error("That path is a folder, not an image: " + path.string());
        return;
    }
    if (!std::filesystem::is_regular_file(target, error) || error) {
        report_error("That path is not a readable image file: " + path.string());
        return;
    }
    const std::uintmax_t size = std::filesystem::file_size(target, error);
    if (error) {
        report_error("That image could not be measured: " + path.string());
        return;
    }
    if (size == 0U) {
        report_error("That image file is empty: " + path.string());
        return;
    }
    if (size > service_detail::maximum_background_bytes) {
        report_error("That image is larger than 4 MB: " + path.string());
        return;
    }
    std::string bytes;
    try {
        bytes = read_file(target, service_detail::maximum_background_bytes);
    } catch (const std::exception&) {
        report_error("That image could not be read");
        return;
    }
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (std::size_t index = 0; index < bytes.size(); index += 3U) {
        const std::size_t remaining = bytes.size() - index;
        const unsigned int first = static_cast<unsigned char>(bytes[index]);
        const unsigned int second = remaining > 1U ? static_cast<unsigned char>(bytes[index + 1U]) : 0U;
        const unsigned int third = remaining > 2U ? static_cast<unsigned char>(bytes[index + 2U]) : 0U;
        encoded.push_back(table[first >> 2U]);
        encoded.push_back(table[((first & 0x03U) << 4U) | (second >> 4U)]);
        encoded.push_back(remaining > 1U ? table[((second & 0x0FU) << 2U) | (third >> 6U)] : '=');
        encoded.push_back(remaining > 2U ? table[third & 0x3FU] : '=');
    }
    const std::string data_url = "data:" + *mime + ";base64," + encoded;
    const std::lock_guard<std::mutex> lock(background_mutex_);
    if (data_url != chat_background_data_url_) {
        chat_background_data_url_ = data_url;
    }
    chat_background_source_ = target.string();
    background_dirty_.store(true);
    remember_background_path(target);
    report_status("Chat background updated");
    send_snapshot();
}

void Service::send_background_candidates() {
    // Offering the pictures already on the machine is far more reliable than making
    // someone type an absolute path and getting it subtly wrong.
    nlohmann::json candidates = nlohmann::json::array();
    static const std::vector<std::string> folders = {"Pictures", "Desktop", "Downloads", "Images", "Wallpapers"};
    for (const std::string& folder : folders) {
        const std::filesystem::path directory = home_directory() / folder;
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error) || error) {
            continue;
        }
        std::vector<std::pair<std::uintmax_t, std::filesystem::path>> found;
        for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
            if (error) {
                break;
            }
            const std::filesystem::path& candidate = entry.path();
            std::error_code file_error;
            if (!service_detail::background_mime_type(candidate).has_value() || !std::filesystem::is_regular_file(candidate, file_error) || file_error) {
                continue;
            }
            const std::uintmax_t size = std::filesystem::file_size(candidate, file_error);
            if (file_error || size == 0U || size > service_detail::maximum_background_bytes) {
                continue;
            }
            found.emplace_back(size, candidate);
        }
        std::sort(found.begin(), found.end(), [](const auto& left, const auto& right) { return left.first > right.first; });
        for (const auto& item : found) {
            if (candidates.size() >= 60U) {
                break;
            }
            candidates.push_back(item.second.string());
        }
    }
    const std::lock_guard<std::mutex> lock(background_mutex_);
    if (background_candidates_ != candidates) {
        background_candidates_ = candidates;
        background_dirty_.store(true);
    }
    send_snapshot();
}

void Service::clear_chat_background() {
    const std::lock_guard<std::mutex> lock(background_mutex_);
    chat_background_data_url_.clear();
    chat_background_source_.clear();
    background_dirty_.store(true);
    remember_background_path({});
    send_snapshot();
}

void Service::remember_background_path(const std::filesystem::path& path) {
    if (!store_.has_value()) {
        return;
    }
    // The choice has to outlive the service, otherwise a restart silently drops the
    // background and the user has to set it again.
    nlohmann::json stored = nlohmann::json::object();
    stored["version"] = 1;
    stored["path"] = path.empty() ? std::string() : path.string();
    try {
        write_json_file_atomically(store_->root() / "chat-background.json", stored, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    } catch (const std::exception&) {
    }
}

void Service::load_remembered_background() {
    if (!store_.has_value()) {
        return;
    }
    const std::filesystem::path settings_file = store_->root() / "chat-background.json";
    std::error_code error;
    if (!std::filesystem::is_regular_file(settings_file, error) || error) {
        return;
    }
    nlohmann::json stored;
    try {
        stored = read_json_file(settings_file, 64U * 1024U);
    } catch (const std::exception&) {
        return;
    }
    const std::string path = stored.value("path", std::string());
    if (path.empty()) {
        return;
    }
    try {
        set_chat_background(nlohmann::json{{"path", path}});
    } catch (const std::exception&) {
    }
}

bool Service::chat_background(std::string& data_url, std::string& source) const {
    const std::lock_guard<std::mutex> lock(background_mutex_);
    data_url = chat_background_data_url_;
    source = chat_background_source_;
    return !data_url.empty();
}

bool Service::credential_present(const std::string& id) {
    if (!store_.has_value()) {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(store_->profile_auth(id), error) && !error;
}

void Service::switch_profile(const std::string& id) {
    if (!store_.has_value() || !cdp_.has_value() || !cdp_->running()) {
        return;
    }
    const std::lock_guard<std::mutex> operation_lock(account_operation_mutex_);
    const auto previous = store_->active();
    if (!previous.has_value() || previous->id == id) {
        return;
    }
    report_status("Switching account…");
    transitioning_.store(true);
    browser_running_.store(false);
    cdp_->close();
    cdp_.reset();
    clear_sessions();
    bool live_replaced = false;
    try {
        if (const auto pid = running_unmanaged_chatgpt(own_process_id()); pid.has_value()) {
            throw Error("unmanaged_codex_running", "Another Codex process is still running");
        }
        store_->capture_live_auth(previous->id);
        store_->activate(id);
        live_replaced = true;
        connect_browser();
    } catch (const std::exception&) {
        if (cdp_.has_value()) {
            cdp_->close();
            cdp_.reset();
        }
        clear_sessions();
        browser_running_.store(false);
        try {
            if (live_replaced) {
                store_->restore_live_auth(previous->id);
                store_->activate(previous->id);
            }
            connect_browser();
        } catch (const std::exception&) {
            request_stop();
        }
        report_error("The account switch could not be completed");
    }
}

void Service::remove_profile(const std::string& id) {
    if (!store_.has_value()) {
        return;
    }
    const std::lock_guard<std::mutex> operation_lock(account_operation_mutex_);
    const auto target = store_->find(id);
    if (!target.has_value()) {
        report_error("That account is no longer stored");
        send_snapshot();
        return;
    }
    const auto active = store_->active();
    const bool was_active = active.has_value() && active->id == id;
    if (!was_active) {
        try {
            const RemovalResult result = store_->remove(id);
            report_status("Removed " + result.removed_label);
        } catch (const Error&) {
            report_error("The stored account could not be removed");
        } catch (const std::exception&) {
            report_error("The stored account could not be removed");
        }
        send_snapshot();
        return;
    }
    if (!cdp_.has_value() || !cdp_->running()) {
        report_error("Reopen the account in Codex before removing it");
        return;
    }
    report_status("Signing out and removing " + target->label + "…");
    transitioning_.store(true);
    browser_running_.store(false);
    cdp_->close();
    cdp_.reset();
    clear_sessions();
    try {
        if (const auto pid = running_unmanaged_chatgpt(own_process_id()); pid.has_value()) {
            throw Error("unmanaged_codex_running", "Another Codex process is still running");
        }
        const RemovalResult result = store_->remove(id);
        connect_browser();
        report_status(result.replacement_active.has_value() ? "Removed " + result.removed_label + " and switched to another account" : "Removed " + result.removed_label + " and signed out");
    } catch (const std::exception&) {
        if (cdp_.has_value()) {
            cdp_->close();
            cdp_.reset();
        }
        clear_sessions();
        browser_running_.store(false);
        try {
            connect_browser();
        } catch (const std::exception&) {
            request_stop();
        }
        report_error("The account could not be removed");
    }
    send_snapshot();
}

void Service::report_error(std::string message) {
    // Always reach the service log first. The renderer is the wrong place to discover a
    // problem, because the most likely time to have one is before a renderer exists,
    // and then the message went nowhere at all.
    std::fputs(("swapdex: " + message + "\n").c_str(), stderr);
    std::fflush(stderr);
    const std::string primary = primary_session_id();
    if (!cdp_.has_value() || !cdp_->running() || primary.empty()) {
        return;
    }
    const std::string expression = "window.__swapdexShowStatus && window.__swapdexShowStatus(" + nlohmann::json(message).dump() + ")";
    try {
        if (const std::string primary = primary_session_id(); !primary.empty()) {
            cdp_->request("Runtime.evaluate", {{"expression", expression}, {"returnByValue", true}}, primary, std::chrono::seconds(5));
        }
    } catch (const Error&) {
    }
}

void Service::report_status(std::string message) {
    report_error(std::move(message));
}

std::string Service::injection_source() const {
    const std::filesystem::path executable_path = std::filesystem::path(executable_directory());
    const std::filesystem::path data_home = std::filesystem::path(environment_value("XDG_DATA_HOME").value_or((home_directory() / ".local" / "share").string()));
    const std::array<std::filesystem::path, 5> candidates = {
        // A downloaded bundle is a flat folder, so the asset sits beside the binary.
        executable_path / "inject.js",
        data_home / "swapdex" / "inject.js",
        executable_path / ".." / "share" / "swapdex" / "inject.js",
        executable_path / ".." / "assets" / "inject.js",
        std::filesystem::path(SWAPDEX_INJECT_SCRIPT_PATH),
    };
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            return read_file(candidate, 2U * 1024U * 1024U);
        }
    }
    throw Error("injection_asset_missing", "The Swapdex renderer integration is missing");
}

// Every page the app owns, whatever its path. The main window and the settings window
// are separate targets, and requiring exactly one match meant only the main window was
// ever injected, so the whole settings section was missing on macOS.
std::vector<std::string> app_page_target_ids(const nlohmann::json& targets) {
    std::vector<std::string> ids;
    if (!targets.is_object() || !targets.contains("targetInfos") || !targets.at("targetInfos").is_array()) {
        return ids;
    }
    for (const nlohmann::json& target : targets.at("targetInfos")) {
        if (!target.is_object() || !target.contains("type") || !target.at("type").is_string() || target.at("type").get<std::string>() != "page"
            || !target.contains("url") || !target.at("url").is_string() || !target.contains("targetId") || !target.at("targetId").is_string()) {
            continue;
        }
        if (target.contains("attached") && target.at("attached").is_boolean() && target.at("attached").get<bool>()) {
            continue;
        }
        std::string url = target.at("url").get<std::string>();
        const std::size_t suffix = url.find_first_of("?#");
        if (suffix != std::string::npos) {
            url.resize(suffix);
        }
        if (url.rfind("app://-/", 0) != 0U) {
            continue;
        }
        ids.push_back(target.at("targetId").get<std::string>());
    }
    return ids;
}

std::vector<std::string> Service::find_targets(const nlohmann::json& targets) const {
    return app_page_target_ids(targets);
}

void Service::acquire_singleton_lock() {
    if (singleton_lock_ || !store_.has_value()) {
        return;
    }
    auto lock = std::make_shared<platform::InstanceLock>(store_->root() / "service.lock");
    if (!lock->acquired()) {
        throw Error("service_already_running", "Another Swapdex service is already running");
    }
    singleton_lock_ = lock;
    store_->set_singleton_lock(lock);
}

void Service::release_singleton_lock() {
    singleton_lock_.reset();
}

namespace {

// The signal handler may only do async signal safe work, so it does nothing but poke
// this pipe. The signal thread does the real work. Using a pipe rather than
// sigtimedwait or sigwait means the wait always has a timeout, so a stop request can
// never be missed because no signal happened to arrive.
std::atomic<int> stop_pipe_write_end{-1};

void handle_stop_signal(int) {
    const int descriptor = stop_pipe_write_end.load();
    if (descriptor >= 0) {
        const char byte = 1;
        const ssize_t written = ::write(descriptor, &byte, 1);
        static_cast<void>(written);
    }
}

}

void Service::start_signal_thread() {
    int descriptors[2] = {-1, -1};
    if (!platform::make_close_on_exec_pipe(descriptors)) {
        throw Error("signal_setup_failed", "Unable to create the service shutdown channel");
    }
    signal_pipe_read_ = descriptors[0];
    signal_pipe_write_ = descriptors[1];
    stop_pipe_write_end.store(descriptors[1]);

    struct sigaction action {};
    action.sa_handler = handle_stop_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (sigaction(SIGINT, &action, nullptr) != 0 || sigaction(SIGTERM, &action, nullptr) != 0) {
        close_signal_pipe();
        throw Error("signal_setup_failed", "Unable to configure service shutdown handling");
    }

    signal_thread_ = std::thread([this] {
        while (!stopping_.load()) {
            struct pollfd entry {};
            entry.fd = signal_pipe_read_;
            entry.events = POLLIN;
            const int ready = ::poll(&entry, 1, 250);
            if (ready > 0) {
                if ((entry.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
                    char drain[64] = {0};
                    const ssize_t drained = ::read(signal_pipe_read_, drain, sizeof(drain));
                    static_cast<void>(drained);
                    request_stop();
                }
            } else if (ready < 0 && errno != EINTR) {
                request_stop();
            }
        }
    });
}

void Service::close_signal_pipe() {
    for (int* descriptor : {&signal_pipe_read_, &signal_pipe_write_}) {
        if (*descriptor >= 0) {
            ::close(*descriptor);
            *descriptor = -1;
        }
    }
    stop_pipe_write_end.store(-1);
}

void Service::stop_background_threads() {
    request_stop();
    if (maintenance_thread_.joinable()) {
        maintenance_thread_.join();
    }
    if (ui_thread_.joinable()) {
        ui_thread_.join();
    }
    if (signal_thread_.joinable()) {
        signal_thread_.join();
    }
    close_signal_pipe();
}

}
