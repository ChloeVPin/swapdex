#pragma once

#include <string>
#include <vector>

namespace swapdex {

// Interactive account picker. Draws its own screen, lets an account be pressed to
// switch, and can relaunch the Codex command line tool as the chosen account.
int tui_command(const std::vector<std::string>& arguments);

}
