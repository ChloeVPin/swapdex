#include "shell.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <memory>

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

    std::filesystem::path profile_home(const std::string& id) const {
        return state_root / "profiles" / id;
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
    ProfileStore store(runtime->state_root, runtime->codex_home, runtime->electron_user_data);
    StoreView view;
    view.state_root = runtime->state_root;
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

AccountMatch resolve_account(const std::string& term) {
    const StoreView store = read_store();
    return resolve_account_in(store.profiles, store.active, term);
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

std::string usage_report(const std::string& id) {
    const StoreView store = read_store();
    const ProfileRecord* record = store.find(id);
    if (record == nullptr) {
        throw Error("profile_not_found", "That account is not stored any more");
    }
    return usage_report_for(*record, store.active);
}

int shell_command(const std::vector<std::string>& arguments) {
    if (!arguments.empty() && (arguments.front() == "--list" || arguments.front() == "-l")) {
        const StoreView store = read_store();
        const std::optional<ProfileRecord>& active = store.active;
        for (const ProfileRecord& record : store.profiles) {
            const bool is_active = active.has_value() && active->id == record.id;
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
            std::cout << "\n";
        }
        return 0;
    }

    const std::string term = arguments.empty() ? std::string() : arguments.front();
    const AccountMatch match = resolve_account(term);
    if (match.ambiguous) {
        std::cerr << "swapdex: more than one account matches that name\n";
        for (const std::string& candidate : match.candidates) {
            std::cerr << "  " << candidate << "\n";
        }
        return 2;
    }
    if (match.id.empty()) {
        std::cerr << "swapdex: no stored account matches that name. Try swapdex shell --list\n";
        return 2;
    }

    const StoreView store = read_store();
    const ProfileRecord* record = store.find(match.id);
    if (record == nullptr || !record->authenticated) {
        std::cerr << "swapdex: that account is not signed in yet. Run swapdex add first.\n";
        return 2;
    }
    std::error_code auth_error;
    if (!std::filesystem::is_regular_file(store.profile_auth(match.id), auth_error) || auth_error) {
        std::cerr << "swapdex: that account has no stored credentials. Run swapdex add first.\n";
        return 2;
    }

    const std::filesystem::path home = store.profile_home(match.id);
    const std::filesystem::path cli = platform::codex_cli_binary();
    if (cli.empty() || !std::filesystem::is_regular_file(cli)) {
        std::cerr << "swapdex: the Codex command line tool was not found on this machine\n";
        return 1;
    }

    std::vector<std::string> forwarded = arguments;
    if (!term.empty()) {
        forwarded.erase(forwarded.begin());
    }
    std::vector<std::string> command;
    command.push_back(platform::to_native(cli));
    for (const std::string& argument : forwarded) {
        command.push_back(argument);
    }
    // The CLI reads its credentials from CODEX_HOME, so pointing it at the stored
    // profile is all it takes to run this session as that account.
    const platform::EnvironmentOverrides overrides = {{"CODEX_HOME", home.string()}};
    return platform::run_command(command, overrides);
}

int usage_command() {
    const StoreView store = read_store();
    if (!store.active.has_value()) {
        std::cout << "No account is signed in.\n";
        return 0;
    }
    std::cout << usage_report(store.active->id);
    return 0;
}

int whoami_command() {
    const StoreView store = read_store();
    if (!store.active.has_value()) {
        std::cout << "No account is signed in to the Codex app.\n";
        return 1;
    }
    std::cout << (store.active->email.has_value() && !store.active->email->empty() ? *store.active->email : store.active->label) << "\n";
    std::cout << "  terminal sessions: swapdex shell " << store.active->id << "\n";
    return 0;
}

}
