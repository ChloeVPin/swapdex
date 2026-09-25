#include "test.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "app_server.hpp"
#include "util.hpp"

void test_app_server_refresh() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() / ("swapdex-app-server-test-" + swapdex::random_identifier(8));
    const std::filesystem::path home = root / "home";
    swapdex::ensure_private_directory(home);
    swapdex::AppServerClient client(SWAPDEX_FAKE_APP_SERVER_PATH);
    const swapdex::AppServerSnapshot snapshot = client.query(home, std::chrono::seconds(5), true);
    swapdex::AppServerClient passive_client(SWAPDEX_FAKE_APP_SERVER_PATH);
    passive_client.query(home, std::chrono::seconds(5));
    swapdex::test::check(snapshot.email.has_value(), "Refresh query did not return an account");
    swapdex::test::check_equal(*snapshot.email, std::string("test@example.test"), "Refresh query returned the wrong account");
    swapdex::test::check(!snapshot.credits_available, "Refresh query incorrectly reported available credits");
    swapdex::test::check(!snapshot.credits_unlimited, "Refresh query incorrectly reported unlimited credits");
    swapdex::test::check_equal(*snapshot.credits_balance, std::string("0"), "Refresh query returned the wrong zero credit balance");
    swapdex::test::check(snapshot.available_reset_credits.has_value(), "Refresh query did not return reset-credit availability");
    swapdex::test::check_equal(*snapshot.available_reset_credits, static_cast<std::int64_t>(3), "Refresh query returned the wrong reset-credit count");
    swapdex::test::check_equal(snapshot.reset_credits.size(), static_cast<std::size_t>(1), "Refresh query did not return reset-credit details");
    swapdex::test::check_equal(snapshot.reset_credits.front().status, std::string("available"), "Refresh query returned an unavailable reset detail");
    swapdex::test::check_equal(*snapshot.reset_credits.front().expires_at, static_cast<std::int64_t>(1893456000), "Refresh query returned the wrong reset expiry");
    swapdex::test::check_equal(snapshot.reset_credits.front().title, std::string("Full reset"), "Refresh query returned the wrong reset title");
    const std::string log = swapdex::read_file(home / "fake-app-server.log", 1024U * 1024U);
    std::size_t account_reads = 0;
    std::size_t true_refreshes = 0;
    std::size_t false_refreshes = 0;
    bool saw_chat_method = false;
    std::size_t start = 0;
    while (start < log.size()) {
        const std::size_t end = log.find('\n', start);
        const std::string line = log.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty()) {
            const nlohmann::json message = nlohmann::json::parse(line);
            const std::string method = message.value("method", "");
            if (method == "account/read") {
                ++account_reads;
                if (message.value("params", nlohmann::json::object()).value("refreshToken", false)) {
                    ++true_refreshes;
                } else {
                    ++false_refreshes;
                }
            }
            if (method.rfind("thread/", 0) == 0 || method.rfind("turn/", 0) == 0) {
                saw_chat_method = true;
            }
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1U;
    }
    swapdex::test::check_equal(account_reads, static_cast<std::size_t>(4), "App-server queries did not perform the expected account reads");
    swapdex::test::check_equal(true_refreshes, static_cast<std::size_t>(2), "Proactive query did not request token refresh for both account reads");
    swapdex::test::check_equal(false_refreshes, static_cast<std::size_t>(2), "Passive query unexpectedly requested token refresh");
    swapdex::test::check(!saw_chat_method, "App-server queries sent a chat or turn method");
    const std::filesystem::path error_home = root / "error-home";
    swapdex::ensure_private_directory(error_home);
    bool auth_error = false;
    try {
        swapdex::AppServerClient error_client(SWAPDEX_FAKE_APP_SERVER_PATH);
        error_client.query(error_home, std::chrono::seconds(5), true);
    } catch (const swapdex::Error& error) {
        auth_error = error.code() == "app_server_auth_required";
    }
    swapdex::test::check(auth_error, "An expired account response did not request re-authentication");
    std::error_code error;
    std::filesystem::remove_all(root, error);
}
