#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "app_server.hpp"
#include "cdp.hpp"
#include "platform.hpp"
#include "test.hpp"
#include "util.hpp"

namespace {

std::filesystem::path test_root() {
    return std::filesystem::temp_directory_path() / ("swapdex-cdp-test-" + swapdex::random_identifier(8));
}

}

// Looks for a Codex command line tool on PATH without running it.
bool codex_command_line_available() {
    const auto path = swapdex::environment_value("PATH");
    if (!path.has_value()) {
        return false;
    }
    const std::string entries(*path);
    std::size_t start = 0;
    while (start <= entries.size()) {
        const std::size_t end = entries.find(':', start);
        const std::string directory = entries.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!directory.empty()) {
#if defined(_WIN32)
            if (std::filesystem::is_regular_file(std::filesystem::path(directory) / "codex.exe")) {
                return true;
            }
#else
            if (std::filesystem::is_regular_file(std::filesystem::path(directory) / "codex")) {
                return true;
            }
#endif
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

void test_json_rpc() {
    const std::filesystem::path root = test_root();
    swapdex::ensure_private_directory(root);
    const std::filesystem::path lock_path = root / "preserved.lock";
    swapdex::write_file_atomically(lock_path, "lock", std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
#if defined(_WIN32)
    // The POSIX test proves a launch does not disturb fd 3. The honest Windows twin
    // is a held lock surviving a child launch: the child must not inherit or close
    // the handle that owns it.
    swapdex::platform::InstanceLock preserved_lock(lock_path);
    swapdex::test::check(preserved_lock.acquired(), "Unable to prepare lock preservation test");
    bool event_received = false;
#else
    const int preserved_descriptor = open(lock_path.c_str(), O_RDWR | O_CLOEXEC);
    int original_three = -1;
    if (fcntl(3, F_GETFD) != -1) {
        original_three = fcntl(3, F_DUPFD_CLOEXEC, 20);
    }
    swapdex::test::check(preserved_descriptor >= 0 && dup2(preserved_descriptor, 3) == 3, "Unable to prepare descriptor preservation test");
    swapdex::test::check(flock(3, LOCK_EX | LOCK_NB) == 0, "Unable to lock preserved descriptor");
    bool event_received = false;
#endif
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
#if defined(_WIN32)
    // The lock was still held while the fake app ran, so a competing lock must
    // still be refused now that it is gone.
    {
        swapdex::platform::InstanceLock competing_lock(lock_path);
        swapdex::test::check(!competing_lock.acquired(), "Preserved lock was lost during the CDP session");
    }
#else
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
#endif
    // This exercises the real Codex app server, so it can only assert anything when a
    // Codex command line tool is actually installed. On a build machine without one the
    // run fails to start, which is a different outcome and not a defect here.
    if (codex_command_line_available()) {
        bool authentication_required = false;
        try {
            swapdex::AppServerClient client;
            client.query(root / "empty-home", std::chrono::seconds(20));
        } catch (const swapdex::Error& error) {
            authentication_required = error.code() == "app_server_auth_required";
        }
        swapdex::test::check(authentication_required, "Empty app-server home did not report missing authentication");
    }
    std::error_code error;
    std::filesystem::remove_all(root, error);
}
