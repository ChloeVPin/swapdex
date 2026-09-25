cmake_minimum_required(VERSION 3.20)
project(swapdex VERSION 0.2.0 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(nlohmann_json 3.11 REQUIRED)
find_package(Threads REQUIRED)

add_library(swapdex_core
    src/app_server.cpp
    src/cdp.cpp
    src/profile_store.cpp
    src/service.cpp
    src/service_control.cpp
    src/util.cpp
)

target_include_directories(swapdex_core PUBLIC src)
target_link_libraries(swapdex_core PUBLIC nlohmann_json::nlohmann_json Threads::Threads)
target_compile_options(swapdex_core PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_compile_definitions(swapdex_core PUBLIC
    SWAPDEX_VERSION="${PROJECT_VERSION}"
    SWAPDEX_CODEX_BINARY="/usr/lib/chatgpt/resources/codex"
    SWAPDEX_CHATGPT_BINARY="/usr/lib/chatgpt/ChatGPT"
)
target_compile_definitions(swapdex_core PRIVATE
    SWAPDEX_INJECT_SCRIPT_PATH="${PROJECT_SOURCE_DIR}/assets/inject.js"
)

add_executable(swapdex src/main.cpp)
target_link_libraries(swapdex PRIVATE swapdex_core)
target_compile_options(swapdex PRIVATE -Wall -Wextra -Wpedantic -Werror)

include(CTest)

if(BUILD_TESTING)
    add_executable(swapdex_fake_cdp tests/fake_cdp.cpp)
    target_compile_options(swapdex_fake_cdp PRIVATE -Wall -Wextra -Wpedantic -Werror)

    add_executable(swapdex_fake_app_server tests/fake_app_server.cpp)
    target_link_libraries(swapdex_fake_app_server PRIVATE nlohmann_json::nlohmann_json)
    target_compile_options(swapdex_fake_app_server PRIVATE -Wall -Wextra -Wpedantic -Werror)

    add_executable(swapdex_tests
        tests/test_main.cpp
        tests/test_profile_store.cpp
        tests/test_json_rpc.cpp
        tests/test_app_server.cpp
        tests/test_service_control.cpp
    )
    target_link_libraries(swapdex_tests PRIVATE swapdex_core)
    target_compile_definitions(swapdex_tests PRIVATE SWAPDEX_FAKE_CDP_PATH="$<TARGET_FILE:swapdex_fake_cdp>" SWAPDEX_FAKE_APP_SERVER_PATH="$<TARGET_FILE:swapdex_fake_app_server>")
    target_compile_options(swapdex_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_dependencies(swapdex_tests swapdex_fake_cdp swapdex_fake_app_server)
    add_test(NAME profile_store COMMAND swapdex_tests profile_store)
    add_test(NAME json_rpc COMMAND swapdex_tests json_rpc)
    add_test(NAME app_server_refresh COMMAND swapdex_tests app_server_refresh)
    add_test(NAME service_control COMMAND swapdex_tests service_control)
    add_test(NAME maintenance_schedule COMMAND swapdex_tests maintenance_schedule)
    add_test(NAME service_control_exec COMMAND swapdex_tests service_control_exec)
    add_test(NAME cli_help COMMAND swapdex --help)
    add_test(NAME cli_version COMMAND swapdex version)
    add_test(NAME cli_invalid COMMAND swapdex invalid-command)
    set_tests_properties(profile_store json_rpc app_server_refresh service_control maintenance_schedule service_control_exec cli_help cli_version cli_invalid PROPERTIES TIMEOUT 30)
    set_tests_properties(cli_help PROPERTIES PASS_REGULAR_EXPRESSION "swapdex install")
    set_tests_properties(cli_version PROPERTIES PASS_REGULAR_EXPRESSION "0\\.2\\.0")
    set_tests_properties(cli_invalid PROPERTIES WILL_FAIL TRUE)

    add_executable(swapdex_live_probe EXCLUDE_FROM_ALL tests/live_cdp_probe.cpp)
    target_link_libraries(swapdex_live_probe PRIVATE swapdex_core)
    target_compile_options(swapdex_live_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

install(TARGETS swapdex RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES assets/inject.js DESTINATION ${CMAKE_INSTALL_DATADIR}/swapdex)
