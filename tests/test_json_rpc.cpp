#include <chrono>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <sys/file.h>
#include <thread>
#include <unistd.h>

#include "app_server.hpp"
#include "cdp.hpp"
#include "test.hpp"
#include "util.hpp"

namespace {

std::filesystem::path test_root() {
    return std::filesystem::temp_directory_path() / ("swapdex-cdp-test-" + swapdex::random_identifier(8));
}

}

void test_json_rpc() {
    const std::filesystem::path root = test_root();
    swapdex::ensure_private_directory(root);
    const std::filesystem::path lock_path = root / "preserved.lock";
    swapdex::write_file_atomically(lock_path, "lock", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
    const int preserved_descriptor = open(lock_path.c_str(), O_RDWR | O_CLOEXEC);
    int original_three = -1;
    if (fcntl(3, F_GETFD) != -1) {
        original_three = fcntl(3, F_DUPFD_CLOEXEC, 20);
    }
    swapdex::test::check(preserved_descriptor >= 0 && dup2(preserved_descriptor, 3) == 3, "Unable to prepare descriptor preservation test");
    swapdex::test::check(flock(3, LOCK_EX | LOCK_NB) == 0, "Unable to lock preserved descriptor");
    bool event_received = false;
    {
        swapdex::CdpPipe pipe(SWAPDEX_FAKE_CDP_PATH, root / "codex", root / "electron");
        pipe.set_event_handler([&event_received](const nlohmann::json& event) {
            if (event.value("method", "") == "Test.event" && event.value("payload", "") == "ok") {
                event_received = true;
            }
        });
        pipe.start();
        nlohmann::json first;
        try {
            first = pipe.request("Test.first", nlohmann::json::object(), std::nullopt, std::chrono::seconds(2));
        } catch (const swapdex::Error& error) {
            throw swapdex::test::Failure(std::string("First CDP request failed: ") + error.code());
        }
        swapdex::test::check_equal(first.at("value").get<int>(), 1, "First CDP response was invalid");
        nlohmann::json second;
        try {
            second = pipe.request("Test.second", nlohmann::json::object(), std::nullopt, std::chrono::seconds(2));
        } catch (const swapdex::Error& error) {
            throw swapdex::test::Failure(std::string("Second CDP request failed: ") + error.code());
        }
        swapdex::test::check_equal(second.at("value").get<int>(), 2, "Second CDP response was invalid");
        for (int attempt = 0; attempt < 20 && !event_received; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        swapdex::test::check(event_received, "CDP event was not delivered");
    }
    swapdex::test::check(fcntl(3, F_GETFD) != -1, "CDP launch closed inherited descriptor 3");
    const int competing_lock = open(lock_path.c_str(), O_RDWR | O_CLOEXEC);
    swapdex::test::check(competing_lock >= 0 && flock(competing_lock, LOCK_EX | LOCK_NB) != 0, "Preserved descriptor lock was lost");
    if (competing_lock >= 0) {
        close(competing_lock);
    }
    flock(3, LOCK_UN);
    close(3);
    if (original_three >= 0) {
        dup2(original_three, 3);
        close(original_three);
    }
    close(preserved_descriptor);
    bool authentication_required = false;
    try {
        swapdex::AppServerClient client;
        client.query(root / "empty-home", std::chrono::seconds(20));
    } catch (const swapdex::Error& error) {
        authentication_required = error.code() == "app_server_auth_required";
    }
    swapdex::test::check(authentication_required, "Empty app-server home did not report missing authentication");
    std::error_code error;
    std::filesystem::remove_all(root, error);
}
