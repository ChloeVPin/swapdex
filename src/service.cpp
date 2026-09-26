#include "service.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <iostream>
#include <poll.h>
#include <pthread.h>
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
    if (const auto pid = running_unmanaged_chatgpt(-1); pid.has_value()) {
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
        if (cdp_.has_value() && cdp_->running() && !active_session_id_.empty()) {
            try {
                const nlohmann::json result = cdp_->request("Runtime.evaluate", {{"expression", "window.__swapdexTakePendingRequest && window.__swapdexTakePendingRequest()"}, {"returnByValue", true}}, active_session_id_, std::chrono::seconds(1));
                if (result.contains("result") && result.at("result").is_object() && result.at("result").contains("value") && result.at("result").at("value").is_string()) {
                    std::string candidate = result.at("result").at("value").get<std::string>();
                    if (!candidate.empty() && candidate.size() <= maximum_ui_payload_bytes) {
                        payload = std::move(candidate);
                    }
                }
            } catch (const Error& error) {
                if (error.code() == "cdp_closed" || stopping_.load()) {
                    return;
                }
            } catch (const std::exception&) {
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

void Service::connect_browser() {
    if (!store_.has_value() || !app_server_.has_value()) {
        throw Error("service_not_initialized", "Swapdex is not initialized");
    }
    if (const auto pid = running_unmanaged_chatgpt(-1); pid.has_value()) {
        throw Error("unmanaged_codex_running", "Close the normally launched Codex application before switching");
    }
    ensure_private_directory(store_->electron_user_data());
    cdp_.emplace(chatgpt_binary(), store_->shared_codex_home(), store_->electron_user_data());
    cdp_->set_event_handler([this](const nlohmann::json& event) {
        if (!event.is_object() || !event.contains("method") || !event.at("method").is_string()) {
            return;
        }
        if (event.at("method").get<std::string>() == "Swapdex.pipeClosed" && !transitioning_.load()) {
            browser_running_.store(false);
        }
    });
    try {
        cdp_->start();
        cdp_->request("Target.setDiscoverTargets", {{"discover", true}}, std::nullopt, std::chrono::seconds(5));
        nlohmann::json target;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (!stopping_.load() && std::chrono::steady_clock::now() < deadline) {
            const nlohmann::json targets = cdp_->request("Target.getTargets", nlohmann::json::object(), std::nullopt, std::chrono::seconds(2));
            const std::string selected = find_target(targets);
            if (!selected.empty()) {
                target = nlohmann::json::parse(selected);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (target.is_null()) {
            throw Error("cdp_target_missing", "The Codex renderer could not be identified");
        }
        if (!target.contains("targetId") || !target.at("targetId").is_string()) {
            throw Error("cdp_target_invalid", "The Codex renderer target is invalid");
        }
        const std::string target_id = target.at("targetId").get<std::string>();
        const nlohmann::json attached = cdp_->request("Target.attachToTarget", {{"targetId", target_id}, {"flatten", true}}, std::nullopt, std::chrono::seconds(5));
        if (!attached.contains("sessionId") || !attached.at("sessionId").is_string()) {
            throw Error("cdp_target_invalid", "The Codex renderer session is invalid");
        }
        active_session_id_ = attached.at("sessionId").get<std::string>();
        cdp_->request("Runtime.enable", nlohmann::json::object(), active_session_id_, std::chrono::seconds(5));
        cdp_->request("Page.enable", nlohmann::json::object(), active_session_id_, std::chrono::seconds(5));
        const std::string source = injection_source();
        cdp_->request("Page.addScriptToEvaluateOnNewDocument", {{"source", source}}, active_session_id_, std::chrono::seconds(10));
        cdp_->request("Runtime.evaluate", {{"expression", source}, {"returnByValue", true}, {"awaitPromise", true}}, active_session_id_, std::chrono::seconds(10));
        const nlohmann::json verification = cdp_->request("Runtime.evaluate", {{"expression", "window.__swapdexInstalled === true && typeof window.__swapdexApplySnapshot === 'function' && typeof window.__swapdexTakePendingRequest === 'function'"}, {"returnByValue", true}}, active_session_id_, std::chrono::seconds(5));
        if (!verification.contains("result") || !verification.at("result").is_object() || !verification.at("result").contains("value") || !verification.at("result").at("value").is_boolean() || !verification.at("result").at("value").get<bool>()) {
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
        active_session_id_.clear();
        transitioning_.store(false);
        browser_running_.store(false);
        throw;
    } catch (const std::exception&) {
        if (cdp_.has_value()) {
            cdp_->close();
            cdp_.reset();
        }
        active_session_id_.clear();
        transitioning_.store(false);
        browser_running_.store(false);
        throw Error("cdp_protocol", "The browser control exchange was invalid");
    }
}

void Service::send_snapshot() {
    if (!cdp_.has_value() || !cdp_->running() || active_session_id_.empty() || !store_.has_value()) {
        return;
    }
    const auto active = store_->active();
    nlohmann::json payload = nlohmann::json::object();
    payload["v"] = 1;
    payload["active"] = active.has_value() ? active->id : "";
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
    cdp_->request("Runtime.evaluate", {{"expression", expression}, {"returnByValue", true}}, active_session_id_, std::chrono::seconds(5));
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
        active_session_id_.clear();
    }
    if (const auto pid = running_unmanaged_chatgpt(-1); pid.has_value()) {
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
    while (onboarding.running() && !stopping_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    onboarding.close();
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
    active_session_id_.clear();
    bool live_replaced = false;
    try {
        if (const auto pid = running_unmanaged_chatgpt(-1); pid.has_value()) {
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
        active_session_id_.clear();
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
    active_session_id_.clear();
    try {
        if (const auto pid = running_unmanaged_chatgpt(-1); pid.has_value()) {
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
        active_session_id_.clear();
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
    if (!cdp_.has_value() || !cdp_->running() || active_session_id_.empty()) {
        return;
    }
    const std::string expression = "window.__swapdexShowStatus && window.__swapdexShowStatus(" + nlohmann::json(message).dump() + ")";
    try {
        cdp_->request("Runtime.evaluate", {{"expression", expression}, {"returnByValue", true}}, active_session_id_, std::chrono::seconds(5));
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

std::string Service::find_target(const nlohmann::json& targets) const {
    if (!targets.is_object() || !targets.contains("targetInfos") || !targets.at("targetInfos").is_array()) {
        return {};
    }
    std::vector<nlohmann::json> matches;
    for (const nlohmann::json& target : targets.at("targetInfos")) {
        if (!target.is_object() || !target.contains("type") || !target.at("type").is_string() || target.at("type").get<std::string>() != "page" || !target.contains("url") || !target.at("url").is_string()) {
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
        if (url == "app://-/index.html") {
            matches.push_back(target);
        }
    }
    return matches.size() == 1U ? matches.front().dump() : std::string();
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
        static_cast<void>(::write(descriptor, &byte, 1));
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
                    static_cast<void>(::read(signal_pipe_read_, drain, sizeof(drain)));
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
