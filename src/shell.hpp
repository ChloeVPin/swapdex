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


// Formatting and matching over a supplied registry, exposed so the logic can be
// tested without an installed service.
AccountMatch resolve_account_in(const std::vector<ProfileRecord>& profiles, const std::optional<ProfileRecord>& active, const std::string& term);
std::string usage_report_for(const ProfileRecord& record, const std::optional<ProfileRecord>& active);
std::string usage_report(const std::string& id);

int accounts_command();

}
