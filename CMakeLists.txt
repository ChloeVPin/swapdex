cmake_minimum_required(VERSION 3.20)
project(swapdex VERSION 0.2.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Use a system nlohmann_json when there is one, and fetch the single header when there
# is not, so a source build works the same on all three systems without asking anyone
# to install a development package first.
find_package(nlohmann_json 3.11 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    set(JSON_BuildTests OFF)
    set(JSON_Install OFF)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
        URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
        DOWNLOAD_EXTRACT_TIMESTAMP OFF
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()
find_package(Threads REQUIRED)

add_library(swapdex_core
    src/app_server.cpp
    src/cdp.cpp
    src/platform.cpp
    src/profile_store.cpp
    src/service.cpp
    src/service_backend.cpp
    src/service_control.cpp
    src/shell.cpp
    src/util.cpp
)

target_include_directories(swapdex_core PUBLIC src)
target_link_libraries(swapdex_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
if(MSVC)
    target_compile_options(swapdex_core PRIVATE /W4 /WX)
else()
    target_compile_options(swapdex_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
target_compile_definitions(swapdex_core PUBLIC
    SWAPDEX_VERSION="${PROJECT_VERSION}"
)

# Both binaries are discovered at run time, next to the app or on PATH, so one build
# serves every machine. Baking in absolute paths made the build machine's layout the
# only one that worked, and the CLI app server silently stopped answering elsewhere.
if(WIN32)
    target_compile_definitions(swapdex_core PUBLIC SWAPDEX_CHATGPT_BINARY="Codex.exe")
elseif(APPLE)
    target_compile_definitions(swapdex_core PUBLIC SWAPDEX_CHATGPT_BINARY="Codex")
else()
    target_compile_definitions(swapdex_core PUBLIC SWAPDEX_CHATGPT_BINARY="ChatGPT")
endif()

if(WIN32)
    target_link_libraries(swapdex_core PUBLIC advapi32)
endif()
target_compile_definitions(swapdex_core PRIVATE
    SWAPDEX_INJECT_SCRIPT_PATH="${PROJECT_SOURCE_DIR}/assets/inject.js"
)

add_executable(swapdex src/main.cpp)
target_link_libraries(swapdex PRIVATE swapdex_core)
if(MSVC)
    target_compile_options(swapdex PRIVATE /W4 /WX)
else()
    target_compile_options(swapdex PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

include(CTest)

if(BUILD_TESTING)
    add_executable(swapdex_fake_cdp tests/fake_cdp.cpp)
    if(MSVC)
    target_compile_options(swapdex_fake_cdp PRIVATE /W4 /WX)
else()
    target_compile_options(swapdex_fake_cdp PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

    add_executable(swapdex_fake_app_server tests/fake_app_server.cpp)
    target_link_libraries(swapdex_fake_app_server PRIVATE nlohmann_json::nlohmann_json)
    if(MSVC)
    target_compile_options(swapdex_fake_app_server PRIVATE /W4 /WX)
else()
    target_compile_options(swapdex_fake_app_server PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

    add_executable(swapdex_tests
        tests/test_main.cpp
        tests/test_profile_store.cpp
        tests/test_json_rpc.cpp
        tests/test_app_server.cpp
        tests/test_service_control.cpp
    )
    target_link_libraries(swapdex_tests PRIVATE swapdex_core)
    target_compile_definitions(swapdex_tests PRIVATE SWAPDEX_FAKE_CDP_PATH="$<TARGET_FILE:swapdex_fake_cdp>" SWAPDEX_FAKE_APP_SERVER_PATH="$<TARGET_FILE:swapdex_fake_app_server>")
    if(MSVC)
    target_compile_options(swapdex_tests PRIVATE /W4 /WX)
else()
    target_compile_options(swapdex_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
    add_dependencies(swapdex_tests swapdex_fake_cdp swapdex_fake_app_server)
    add_test(NAME profile_store COMMAND swapdex_tests profile_store)
    add_test(NAME json_rpc COMMAND swapdex_tests json_rpc)
    add_test(NAME app_server_refresh COMMAND swapdex_tests app_server_refresh)
    add_test(NAME service_control COMMAND swapdex_tests service_control)
    add_test(NAME maintenance_schedule COMMAND swapdex_tests maintenance_schedule)
    add_test(NAME service_control_exec COMMAND swapdex_tests service_control_exec)
    add_test(NAME platform_layer COMMAND swapdex_tests platform_layer)
    add_test(NAME shutdown_channel COMMAND swapdex_tests shutdown_channel)
    add_test(NAME app_windows COMMAND swapdex_tests app_windows)
    add_test(NAME settings_tabs COMMAND swapdex_tests settings_tabs)
    add_test(NAME shell_bridge COMMAND swapdex_tests shell_bridge)
    add_test(NAME account_root_scope COMMAND swapdex_tests account_root_scope)
    add_test(NAME start_reports_the_truth COMMAND swapdex_tests start_reports_the_truth)
    add_test(NAME release_bundle_layout COMMAND swapdex_tests release_bundle_layout)
    add_test(NAME cli_help COMMAND swapdex --help)
    add_test(NAME cli_version COMMAND swapdex version)
    add_test(NAME cli_invalid COMMAND swapdex invalid-command)
    set_tests_properties(profile_store json_rpc app_server_refresh service_control maintenance_schedule service_control_exec platform_layer shutdown_channel app_windows settings_tabs shell_bridge account_root_scope start_reports_the_truth release_bundle_layout cli_help cli_version cli_invalid PROPERTIES TIMEOUT 30)
    set_tests_properties(cli_help PROPERTIES PASS_REGULAR_EXPRESSION "swapdex install")
    set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "0\\.2\\.0")
    set_tests_properties(cli_invalid PROPERTIES WILL_FAIL TRUE)

    add_executable(swapdex_live_probe EXCLUDE_FROM_ALL tests/live_cdp_probe.cpp)
    target_link_libraries(swapdex_live_probe PRIVATE swapdex_core)
    if(MSVC)
    target_compile_options(swapdex_live_probe PRIVATE /W4 /WX)
else()
    target_compile_options(swapdex_live_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
endif()

install(TARGETS swapdex RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES assets/inject.js DESTINATION ${CMAKE_INSTALL_DATADIR}/swapdex)
