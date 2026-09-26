#include <exception>
#include <iostream>
#include <string>

#include "test.hpp"

void test_profile_store();
void test_json_rpc();
void test_app_server_refresh();
void test_service_control();
void test_maintenance_schedule();
void test_service_control_exec();
void test_platform_layer();
void test_shutdown_channel_is_interruptible();
void test_every_app_window_is_a_target();
void test_shell_bridge();
void test_account_root_is_explicit();
void test_start_reports_the_truth();
void test_release_bundle_layout();

int main(int argc, char** argv) {
    const std::string filter = argc > 1 ? argv[1] : "";
    const std::pair<std::string, swapdex::test::TestFunction> tests[] = {
        {"profile_store", test_profile_store},
        {"json_rpc", test_json_rpc},
        {"app_server_refresh", test_app_server_refresh},
        {"service_control", test_service_control},
        {"maintenance_schedule", test_maintenance_schedule},
        {"service_control_exec", test_service_control_exec},
        {"platform_layer", test_platform_layer},
        {"shutdown_channel", test_shutdown_channel_is_interruptible},
        {"app_windows", test_every_app_window_is_a_target},
        {"shell_bridge", test_shell_bridge},
        {"account_root_scope", test_account_root_is_explicit},
        {"start_reports_the_truth", test_start_reports_the_truth},
        {"release_bundle_layout", test_release_bundle_layout},
    };
    std::size_t failures = 0;
    std::size_t matched = 0;
    for (const auto& [name, test] : tests) {
        if (!filter.empty() && name != filter) {
            continue;
        }
        ++matched;
        try {
            test();
            std::cout << "PASS " << name << "\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << "\n";
        }
    }
    if (!filter.empty() && matched == 0U) {
        std::cerr << "FAIL unknown test filter\n";
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
