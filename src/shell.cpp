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
    ProfileStore store(runtime->state_root, runtime->codex_home, runtime->electron_user_data);
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

int skill_command(const std::vector<std::string>& arguments) {
    const bool remove = std::find(arguments.begin(), arguments.end(), "--remove") != arguments.end();
    const std::filesystem::path home = platform::home_directory();
    const std::filesystem::path directory = home / ".codex" / "skills" / "swapdex";
    const std::filesystem::path target = directory / "SKILL.md";
    if (remove) {
        std::error_code error;
        if (std::filesystem::is_regular_file(target, error) && !error) {
            std::filesystem::remove(target, error);
        }
        std::error_code directory_error;
        if (std::filesystem::is_empty(directory, directory_error) && !directory_error) {
            std::filesystem::remove(directory, directory_error);
        }
        std::cout << "Removed the Swapdex skill from Codex.\n";
        return 0;
    }
    const std::vector<std::filesystem::path> candidates = {
        platform::data_directory() / "swapdex" / "skill" / "SKILL.md",
        platform::executable_directory() / ".." / "share" / "swapdex" / "skill" / "SKILL.md",
        platform::executable_directory() / ".." / "assets" / "skill" / "SKILL.md",
        std::filesystem::path(SWAPDEX_INJECT_SCRIPT_PATH).parent_path().parent_path() / "assets" / "skill" / "SKILL.md",
    };
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error) {
            ensure_directory(directory, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec);
            copy_file_atomically(candidate, target, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
            std::cout << "Installed the Swapdex skill for Codex.\n";
            std::cout << "Start a new Codex session, then type /swapdex or $swapdex.\n";
            return 0;
        }
    }
    std::cerr << "swapdex: the bundled skill file could not be found\n";
    return 1;
}

int shell_command(const std::vector<std::string>& arguments) {
    bool follow = true;
    std::string explicit_account;
    bool account_given = false;
    std::vector<std::string> rest;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        if (argument == "--no-follow") {
            follow = false;
            continue;
        }
        if (argument == "--account" && index + 1U < arguments.size()) {
            explicit_account = arguments[index + 1U];
            account_given = true;
            ++index;
            continue;
        }
        rest.push_back(argument);
    }

    if (!arguments.empty() && (arguments.front() == "--list" || arguments.front() == "-l")) {
        return accounts_command();
    }

    // A single "--" separates Swapdex options from Codex arguments. It is not passed
    // along, otherwise Codex would read it as the end of its own options.
    if (!rest.empty() && rest.front() == "--") {
        rest.erase(rest.begin());
    }

    std::string term = account_given ? explicit_account : std::string();
    if (!account_given && !rest.empty() && rest.front().rfind("--", 0) != 0) {
        // A leading word that names exactly one stored account selects it, which keeps
        // "swapdex shell work" working. Anything else is passed straight to Codex.
        const AccountMatch probe = resolve_account(rest.front());
        if (!probe.id.empty() && !probe.ambiguous) {
            term = rest.front();
            rest.erase(rest.begin());
            if (!rest.empty() && rest.front() == "--") {
                rest.erase(rest.begin());
            }
        }
    }
    const std::vector<std::string> forwarded = rest;

    if (!follow) {
        const std::filesystem::path cli = platform::codex_cli_binary();
        if (cli.empty() || !std::filesystem::is_regular_file(cli)) {
            std::cerr << "swapdex: the Codex command line tool was not found on this machine\n";
            return 1;
        }
        std::vector<std::string> command;
        command.push_back(platform::to_native(cli));
        for (const std::string& argument : forwarded) {
            command.push_back(argument);
        }
        return platform::run_command(command, {});
    }

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

std::string shell_snippet(const std::string& shell) {
    const std::string marker_begin = "# >>> swapdex >>>";
    const std::string marker_end = "# <<< swapdex <<<";
    std::string body;
    body += marker_begin + "\n";
    body += "# Codex runs through Swapdex so terminal sessions use the account signed in to the app.\n";
    body += "# Bypass this for one command with SWAPDEX_FOLLOW=0, or run swapdex shell --no-follow.\n";
    body += "codex() {\n";
    body += "  if [ -n \"${SWAPDEX_FOLLOW:-}\" ]; then\n";
    body += "    command codex \"$@\"\n";
    body += "    return $?\n";
    body += "  fi\n";
    body += "  command swapdex shell \"$@\"\n";
    body += "}\n";
    body += marker_end + "\n";
    static_cast<void>(shell);
    return body;
}

namespace {

std::vector<std::filesystem::path> shell_startup_files(const std::string& shell) {
    const std::filesystem::path home = platform::home_directory();
    if (shell == "zsh") {
        return {home / ".zshrc"};
    }
    if (shell == "bash") {
        return {home / ".bashrc"};
    }
    if (shell == "fish") {
        return {home / ".config" / "fish" / "config.fish"};
    }
    return {};
}

void remove_snippet_from(std::string& contents) {
    const std::string begin = "# >>> swapdex >>>";
    const std::string end = "# <<< swapdex <<<";
    const std::size_t start = contents.find(begin);
    if (start == std::string::npos) {
        return;
    }
    const std::size_t finish = contents.find(end, start);
    if (finish == std::string::npos) {
        return;
    }
    const std::size_t tail = contents.find('\n', finish);
    contents.erase(start, tail == std::string::npos ? contents.size() - start : tail - start + 1U);
}

}

int shell_init_command(const std::vector<std::string>& arguments) {
    const bool uninstall = std::find(arguments.begin(), arguments.end(), "--remove") != arguments.end();
    std::string shell = "bash";
    const std::optional<std::string> from_environment = environment_value("SHELL");
    if (from_environment.has_value()) {
        const std::string name = platform::from_native(*from_environment).filename().string();
        if (name == "zsh" || name == "bash" || name == "fish") {
            shell = name;
        }
    }
    const std::vector<std::filesystem::path> targets = shell_startup_files(shell);
    if (targets.empty()) {
        std::cerr << "swapdex: set SHELL to bash, zsh, or fish and run this again\n";
        return 1;
    }
    const std::filesystem::path target = targets.front();
    std::string contents;
    std::error_code exists_error;
    if (std::filesystem::is_regular_file(target, exists_error) && !exists_error) {
        contents = read_file(target, 4U * 1024U * 1024U);
    }
    remove_snippet_from(contents);
    if (!uninstall) {
        if (!contents.empty() && contents.back() != '\n') {
            contents.push_back('\n');
        }
        contents += "\n" + shell_snippet(shell);
    }
    const std::filesystem::perms permissions = std::filesystem::is_regular_file(target)
        ? std::filesystem::status(target).permissions()
        : (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    if (uninstall) {
        write_file_atomically(target, contents, permissions);
        std::cout << "Removed the Swapdex codex wrapper from " << target.string() << "\n";
        return 0;
    }
    write_file_atomically(target, contents, permissions);
    std::cout << "Added the Swapdex codex wrapper to " << target.string() << "\n";
    std::cout << "Open a new terminal, or run: source " << target.string() << "\n";
    std::cout << "Bypass it any time with SWAPDEX_FOLLOW=0 codex\n";
    return 0;
}

int statusline_command(const std::vector<std::string>& arguments) {
    const bool apply = std::find(arguments.begin(), arguments.end(), "--apply") != arguments.end();
    const std::string recommended = "status_line = [\"five-hour-limit\", \"weekly-limit\"]";
    if (!apply) {
        std::cout << "Codex draws its status line itself, and Swapdex never renders into it.\n";
        std::cout << "The built in limit items already show the right account once the terminal\n";
        std::cout << "runs through swapdex shell, because the CLI is authenticated as that account.\n\n";
        std::cout << "To turn those two items on, either run /statusline inside Codex, or add this\n";
        std::cout << "to the [tui] section of your Codex config:\n\n";
        std::cout << "  " << recommended << "\n\n";
        std::cout << "Or let Swapdex add it for you, keeping a backup: swapdex statusline --apply\n";
        return 0;
    }

    const std::filesystem::path home = platform::home_directory();
    const std::filesystem::path config = home / ".codex" / "config.toml";
    std::error_code error;
    if (!std::filesystem::is_regular_file(config, error) || error) {
        std::cerr << "swapdex: no Codex config was found at " << config.string() << "\n";
        std::cerr << "Start Codex once, then run this again.\n";
        return 1;
    }
    std::string contents = read_file(config, 4U * 1024U * 1024U);
    if (contents.find("status_line") != std::string::npos) {
        std::cerr << "swapdex: your config already sets a status line, so nothing was changed.\n";
        std::cerr << "Edit the [tui] status_line list yourself if you want a different set.\n";
        return 1;
    }
    const std::string backup = config.string() + ".swapdex-backup";
    write_file_atomically(backup, contents, std::filesystem::status(config).permissions());
    if (contents.find("[tui]") != std::string::npos) {
        const std::size_t section = contents.find("[tui]");
        const std::size_t insert = contents.find('\n', section);
        contents.insert(insert + 1U, "  " + recommended + "\n");
    } else {
        if (!contents.empty() && contents.back() != '\n') {
            contents.push_back('\n');
        }
        contents += "\n[tui]\n  " + recommended + "\n";
    }
    write_file_atomically(config, contents, std::filesystem::status(config).permissions());
    std::cout << "Added the rate limit items to your Codex status line.\n";
    std::cout << "A backup of your previous config is at " << backup << "\n";
    std::cout << "Start a new Codex session to see it.\n";
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
