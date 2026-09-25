#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace swapdex {

struct ProfileRecord;

// Resolves a stored account from a user supplied term. Matching is case insensitive
// and accepts an account id, part of an id, a label, or part of an email address.
// Returns an empty string when nothing matches and sets ambiguous when more than one
// account matches.
struct AccountMatch {
    std::string id;
    bool ambiguous = false;
    std::vector<std::string> candidates;
};

AccountMatch resolve_account(const std::string& term);

// Formatting and matching over a supplied registry, exposed so the logic can be
// tested without an installed service.
AccountMatch resolve_account_in(const std::vector<ProfileRecord>& profiles, const std::optional<ProfileRecord>& active, const std::string& term);
std::string usage_report_for(const ProfileRecord& record, const std::optional<ProfileRecord>& active);
std::string usage_report(const std::string& id);

int shell_command(const std::vector<std::string>& arguments);
int accounts_command();
// Codex reads skills from $CODEX_HOME/skills, and the wrapper points CODEX_HOME at an
// account, so every account home needs the skill.
std::vector<std::filesystem::path> skill_install_directories(const std::vector<std::filesystem::path>& account_homes, const std::filesystem::path& shared_home);
int usage_command();
int whoami_command();

// The Codex status line only renders built in segments, so Swapdex never renders into
// it. What Swapdex does is make the built in limit segments correct by choosing the
// account the terminal runs as. These commands print, and optionally apply, the
// segment list that surfaces those limits.
int statusline_command(const std::vector<std::string>& arguments);
int skill_command(const std::vector<std::string>& arguments);
int shell_init_command(const std::vector<std::string>& arguments);

// The shell snippet installed by shell-init, exposed so it can be tested.
std::string shell_snippet(const std::string& shell);

}
