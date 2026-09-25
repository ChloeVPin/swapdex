#include "tui.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <thread>

#include "platform.hpp"
#include "profile_store.hpp"
#include "service_control.hpp"
#include "shell.hpp"
#include "util.hpp"

namespace swapdex {
namespace {

constexpr int poll_milliseconds = 120;
constexpr int action_wait_seconds = 90;

const char* dim() {
    return "\x1b[2m";
}

const char* bold() {
    return "\x1b[1m";
}


const char* green() {
    return "\x1b[32m";
}

std::string row_label(const ProfileRecord& record) {
    std::string text = record.email.has_value() && !record.email->empty() ? *record.email : record.label;
    if (record.plan == "plus" || record.plan == "pro" || record.plan == "team") {
        std::string plan = record.plan;
        plan[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(plan[0])));
        text += "  " + plan;
    }
    if (record.primary_usage.has_value() && record.primary_usage->remaining_percent.has_value()) {
        char buffer[16] = {0};
        std::snprintf(buffer, sizeof(buffer), "  5h %d%%", *record.primary_usage->remaining_percent);
        text += buffer;
    }
    if (record.secondary_usage.has_value() && record.secondary_usage->remaining_percent.has_value()) {
        char buffer[16] = {0};
        std::snprintf(buffer, sizeof(buffer), "  7d %d%%", *record.secondary_usage->remaining_percent);
        text += buffer;
    }
    return text;
}

struct Loaded {
    std::optional<RuntimePaths> runtime;
    std::vector<ProfileRecord> profiles;
    std::optional<ProfileRecord> active;
};

Loaded load() {
    Loaded loaded;
    loaded.runtime = installed_runtime_paths();
    if (!loaded.runtime.has_value()) {
        return loaded;
    }
    ProfileStore store(loaded.runtime->state_root, default_account_root(), loaded.runtime->codex_home, loaded.runtime->electron_user_data);
    loaded.profiles = store.list_existing();
    loaded.active = store.active_existing();
    return loaded;
}

// Asks the running service to act, which keeps a single writer for credentials. If no
// service is running the caller falls back to a direct swap.
bool request_service_action(const Loaded& loaded, const std::string& action, const std::string& key) {
    if (!loaded.runtime.has_value()) {
        return false;
    }
    const std::filesystem::path directory = loaded.runtime->state_root / "control";
    ensure_private_directory(directory);
    const Json payload = {
        {"v", 1},
        {"source", "profile-dropdown"},
        {"action", action},
        {"key", key}
    };
    const std::filesystem::path target = directory / "pending.json";
    const std::filesystem::path temporary = directory / "pending.json.tmp";
    try {
        write_file_atomically(temporary, payload.dump(), std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        std::error_code error;
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary, error);
            return false;
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

bool wait_for_active(const std::string& id) {
    for (int elapsed = 0; elapsed < action_wait_seconds * 10; ++elapsed) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const Loaded current = load();
        if (current.active.has_value() && current.active->id == id) {
            return true;
        }
    }
    return false;
}

void render(const Loaded& loaded, std::size_t selected, const std::string& status) {
    std::string screen;
    screen += "\x1b[H\x1b[2J";
    screen += std::string(bold()) + "Swapdex" + "\x1b[0m\n";
    screen += std::string(dim()) + "Pick an account. Enter switches, a adds, r removes, q quits." + "\x1b[0m\n\n";
    if (loaded.profiles.empty()) {
        screen += "  No accounts stored yet. Press a to add one.\n";
    }
    for (std::size_t index = 0; index < loaded.profiles.size(); ++index) {
        const ProfileRecord& record = loaded.profiles[index];
        const bool is_active = loaded.active.has_value() && loaded.active->id == record.id;
        const bool cursor = index == selected;
        std::string marker = is_active ? std::string(green()) + "* " : "  ";
        screen += cursor ? "> " : "  ";
        screen += marker;
        screen += row_label(record);
        screen += "\x1b[0m\n";
    }
    screen += "\n";
    screen += std::string(dim()) + "  * signed in to the Codex app" + "\x1b[0m\n";
    if (!status.empty()) {
        screen += "\n" + status + "\n";
    }
    platform::write_styled(screen);
}

void report(const std::string& text) {
    std::fputs((text + "\n").c_str(), stdout);
    std::fflush(stdout);
}

int relaunch_cli(const std::string& id) {
    const Loaded loaded = load();
    if (!loaded.runtime.has_value()) {
        report("Swapdex is not installed.");
        return 1;
    }
    ProfileStore store(loaded.runtime->state_root, default_account_root(), loaded.runtime->codex_home, loaded.runtime->electron_user_data);
    const std::filesystem::path home = store.profile_home(id);
    const std::filesystem::path cli = platform::codex_cli_binary();
    if (cli.empty() || !std::filesystem::is_regular_file(cli)) {
        report("The Codex command line tool was not found on this machine.");
        return 1;
    }
    report("Starting Codex as that account. To come back here, run swapdex tui again.");
    return platform::run_command({platform::to_native(cli)}, {{"CODEX_HOME", home.string()}});
}

}

int tui_command(const std::vector<std::string>& arguments) {
    const bool relaunch = std::find(arguments.begin(), arguments.end(), "--no-relaunch") == arguments.end();
    Loaded loaded = load();
    if (!loaded.runtime.has_value()) {
        report("Swapdex is not installed. Run the install command first.");
        return 1;
    }

    platform::RawTerminal terminal;
    platform::enter_alternate_screen();
    std::size_t selected = 0;
    std::string status;
    render(loaded, selected, status);

    int result = 0;
    while (true) {
        const std::string key = terminal.read_key(poll_milliseconds);
        if (key.empty()) {
            continue;
        }
        if (key == "q" || key == "escape" || key == "3") {
            break;
        }
        if (key == "up" || key == "k") {
            if (!loaded.profiles.empty()) {
                selected = (selected + loaded.profiles.size() - 1U) % loaded.profiles.size();
            }
        } else if (key == "down" || key == "j") {
            if (!loaded.profiles.empty()) {
                selected = (selected + 1U) % loaded.profiles.size();
            }
        } else if (key == "a") {
            platform::leave_alternate_screen();
            report("Opening the account sign in flow. Finish it, then run swapdex tui again.");
            result = shell_command({"add"});
            return result;
        } else if (key == "r") {
            if (loaded.profiles.size() <= 1U) {
                status = "Keep at least one account stored.";
            } else if (selected < loaded.profiles.size()) {
                const ProfileRecord& record = loaded.profiles[selected];
                platform::leave_alternate_screen();
                report("About to remove " + (record.email.has_value() ? *record.email : record.label) + ".");
                report("Press y to confirm, anything else to keep it.");
                platform::RawTerminal confirm_terminal;
                std::string answer;
                for (int wait = 0; wait < 100 && answer.empty(); ++wait) {
                    answer = confirm_terminal.read_key(50);
                }
                platform::enter_alternate_screen();
                if (answer == "y" || answer == "Y") {
                    if (request_service_action(loaded, "remove", record.id)) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(600));
                    } else {
                        status = "The service is not running, so start Swapdex before removing accounts.";
                    }
                } else {
                    status = "Kept the account.";
                }
                loaded = load();
                selected = 0;
            }
        } else if (key == "s") {
            status = "Settings live in the Codex app under Settings, Swapdex.";
        } else if (key == "\r" || key == "\n") {
            if (selected >= loaded.profiles.size()) {
                status = "Nothing to switch to.";
            } else {
                const std::string id = loaded.profiles[selected].id;
                if (loaded.active.has_value() && loaded.active->id == id) {
                    status = "Already signed in to that account.";
                } else if (request_service_action(loaded, "switch", id)) {
                    status = "Switching, this takes a moment.";
                    const bool done = wait_for_active(id);
                    status = done ? "Switched." : "The service did not finish switching. Try swapdex status.";
                    loaded = load();
                    selected = 0;
                } else {
                    status = "The Swapdex service is not running, so start it with swapdex start.";
                }
            }
        }
        render(loaded, selected, status);
    }

    platform::leave_alternate_screen();
    if (relaunch && loaded.active.has_value()) {
        return relaunch_cli(loaded.active->id);
    }
    return result;
}

}
