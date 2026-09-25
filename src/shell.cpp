#include "shell.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>

#include "platform.hpp"
#include "profile_store.hpp"
#include "service_control.hpp"
#include "util.hpp"

namespace swapdex {
namespace {

std::string lowered(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

// Every terminal command reads the registry without ever writing to it, so a command
// can never change stored credentials while the service is running.
struct StoreView {
    std::filesystem::path state_root;
    std::vector<ProfileRecord> profiles;
    std::optional<ProfileRecord> active;

    const ProfileRecord* find(const std::string& id) const {
        for (const ProfileRecord& record : profiles) {
            if (record.id == id) {
                return &record;
            }
        }
        return nullptr;
    }

    std::filesystem::path account_root;

    std::filesystem::path profile_home(const std::string& id) const {
        return account_root / id;
    }

    std::filesystem::path profile_auth(const std::string& id) const {
        return profile_home(id) / "auth.json";
    }
};

StoreView read_store() {
    const std::optional<RuntimePaths> runtime = installed_runtime_paths();
    if (!runtime.has_value()) {
        throw Error("service_not_installed", "Swapdex is not installed. Run the install command first.");
    }
    ProfileStore store(runtime->state_root, default_account_root(), runtime->codex_home, runtime->electron_user_data);
    StoreView view;
    view.state_root = runtime->state_root;
    view.account_root = store.account_root();
    view.profiles = store.list_existing();
    view.active = store.active_existing();
    return view;
}

std::string bar(int percent) {
    const int clamped = std::max(0, std::min(100, percent));
    const int filled = (clamped * 10) / 100;
    std::string line = "[";
    for (int index = 0; index < 10; ++index) {
        line += index < filled ? "#" : ".";
    }
    line += "]";
    return line;
}

std::string window_text(const std::optional<UsageWindow>& window, const std::string& label) {
    if (!window.has_value() || !window->remaining_percent.has_value()) {
        return std::string();
    }
    std::string line = "  " + label + "  " + bar(*window->remaining_percent) + "  " + std::to_string(*window->remaining_percent) + "% left";
    if (window->resets_at.has_value() && *window->resets_at > 0) {
        line += ", resets " + format_timestamp(*window->resets_at);
    }
    return line;
}

}

AccountMatch resolve_account_in(const std::vector<ProfileRecord>& profiles, const std::optional<ProfileRecord>& active, const std::string& term) {
    AccountMatch result;
    const std::string needle = lowered(term);
    if (needle.empty()) {
        result.id = active.has_value() ? active->id : std::string();
        return result;
    }
    std::vector<std::string> exact;
    std::vector<std::string> partial;
    for (const ProfileRecord& record : profiles) {
        const std::string id = lowered(record.id);
        const std::string label = lowered(record.label);
        const std::string email = record.email.has_value() ? lowered(*record.email) : std::string();
        const bool is_exact = id == needle || label == needle || (!email.empty() && email == needle);
        const bool is_partial = id.find(needle) != std::string::npos
            || label.find(needle) != std::string::npos
            || (!email.empty() && email.find(needle) != std::string::npos);
        if (is_exact) {
            exact.push_back(record.id);
        } else if (is_partial) {
            partial.push_back(record.id);
        }
    }
    const std::vector<std::string>& hits = exact.empty() ? partial : exact;
    if (hits.size() == 1U) {
        result.id = hits.front();
        return result;
    }
    result.ambiguous = !hits.empty();
    result.candidates = hits;
    return result;
}

std::string usage_report_for(const ProfileRecord& record, const std::optional<ProfileRecord>& active) {
    std::string text;
    text += (record.email.has_value() && !record.email->empty() ? *record.email : record.label) + "\n";
    std::string plan = record.plan;
    if (!plan.empty()) {
        plan[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(plan[0])));
    }
    text += "  plan   " + (plan.empty() ? std::string("unknown") : plan) + "\n";
    if (active.has_value() && active->id == record.id) {
        text += "  state  signed in to the Codex app\n";
    } else {
        text += "  state  stored, not signed in\n";
    }
    const std::string five = window_text(record.primary_usage, "5h   ");
    const std::string weekly = window_text(record.secondary_usage, "7d   ");
    if (!five.empty()) {
        text += five + "\n";
    }
    if (!weekly.empty()) {
        text += weekly + "\n";
    }
    if (record.credits_balance.has_value() && !record.credits_balance->empty()) {
        text += "  credits  " + *record.credits_balance + "\n";
    }
    if (record.available_reset_credits.has_value()) {
        text += "  resets   " + std::to_string(*record.available_reset_credits) + " available\n";
    }
    return text;
}

int accounts_command() {
    const StoreView store = read_store();
    for (const ProfileRecord& record : store.profiles) {
        const bool is_active = store.active.has_value() && store.active->id == record.id;
        std::cout << (is_active ? "* " : "  ")
                  << (record.email.has_value() && !record.email->empty() ? *record.email : record.label);
        if (record.plan == "plus" || record.plan == "pro" || record.plan == "team") {
            std::string plan = record.plan;
            plan[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(plan[0])));
            std::cout << "  " << plan;
        }
        if (record.primary_usage.has_value() && record.primary_usage->remaining_percent.has_value()) {
            std::cout << "  5h " << *record.primary_usage->remaining_percent << "%";
        }
        if (record.secondary_usage.has_value() && record.secondary_usage->remaining_percent.has_value()) {
            std::cout << "  7d " << *record.secondary_usage->remaining_percent << "%";
        }
        if (record.available_reset_credits.has_value() && *record.available_reset_credits > 0) {
            std::cout << "  " << *record.available_reset_credits << " reset";
        }
        if (!record.authenticated) {
            std::cout << "  sign-in required";
        }
        std::cout << "\n";
    }
    if (store.profiles.empty()) {
        std::cout << "No accounts are stored yet.\n";
    }
    return 0;
}

namespace {

}

}
