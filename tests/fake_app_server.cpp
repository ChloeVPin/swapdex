#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

int main() {
    const char* home_value = std::getenv("CODEX_HOME");
    const std::string home = home_value == nullptr ? "" : home_value;
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        nlohmann::json message;
        try {
            message = nlohmann::json::parse(line);
        } catch (...) {
            continue;
        }
        const std::string method = message.value("method", "");
        if (message.contains("id")) {
            nlohmann::json response;
            response["id"] = message.at("id");
            if (method == "initialize") {
                response["result"] = {{"codexHome", home}};
            } else if (method == "account/read" && home.find("error-home") != std::string::npos) {
                response["error"] = {{"code", -32001}, {"message", "authentication expired"}};
            } else if (method == "account/read") {
                response["result"] = {{"account", {{"type", "chatgpt"}, {"email", "test@example.test"}, {"planType", "pro"}, {"workspaceRouting", {{"chatgptAccountId", "account-test"}}}}}};
            } else if (method == "account/rateLimits/read") {
                response["result"] = {{"rateLimits", {{"primary", {{"usedPercent", 10}, {"windowDurationMins", 300}, {"resetsAt", 1}}}, {"secondary", {{"usedPercent", 20}, {"windowDurationMins", 300}, {"resetsAt", 2}}}, {"credits", {{"hasCredits", false}, {"unlimited", false}, {"balance", "0"}}}}}, {"rateLimitResetCredits", {{"availableCount", 3}, {"credits", nlohmann::json::array()}}}};
                response["result"]["rateLimitResetCredits"]["credits"].push_back({{"id", "reset-1"}, {"resetType", "codexRateLimits"}, {"status", "available"}, {"grantedAt", 1700000000}, {"expiresAt", 1893456000}, {"title", "Full reset"}, {"description", "Restores your 5-hour and weekly limits"}});
            } else if (method == "account/usage/read") {
                response["result"] = {{"summary", {{"lifetimeTokens", 42}}}};
            } else {
                response["error"] = {{"code", -32601}, {"message", "method not allowed"}};
            }
            std::cout << response.dump() << "\n" << std::flush;
        }
        if (!home.empty()) {
            std::ofstream log(home + "/fake-app-server.log", std::ios::app);
            log << message.dump() << "\n";
        }
    }
    return 0;
}
