cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
# A build that adds this package as a subproject sets the normal variable
# LOGONOMIC_CLI_ADDON_EMBEDDED. That build owns the cache, try_compile()
# settings, the SDK and testing (the SDK's docs/package_contract.md,
# "Embedded build"), so only a standalone build sets them up here.
if(NOT LOGONOMIC_CLI_ADDON_EMBEDDED)
    set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Do not capture configure environment" FORCE)
    list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
endif()
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
elseif(CMAKE_GENERATOR STREQUAL "FASTBuild")
    message(FATAL_ERROR "FASTBuild package builds require direct FetchContent population (CMP0168).")
endif()
project(logonomic_cli_qwen VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    if(NOT TARGET logonomic_cli_addon_sdk::sdk)
        message(FATAL_ERROR
            "The embedding build must add the Logonomic CLI add-on SDK before this package.")
    endif()
else()
    set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
    set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
        add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/sdk")
    else()
        include(FetchContent)
        FetchContent_Declare(logonomic_sdk_source
            GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
        FetchContent_MakeAvailable(logonomic_sdk_source)
    endif()
endif()

function(qwen_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    # The Windows API selection belongs to the platform, so a MinGW build gets
    # the same one as MSVC.
    if(WIN32)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

add_library(qwen_provider STATIC
    src/qwen_accounts.cpp src/qwen_usage.cpp src/qwen_profile.cpp
    src/qwen_acp.cpp src/qwen_config.cpp src/qwen_launcher.cpp
    src/qwen_lifecycle.cpp src/qwen_preparation.cpp
    src/qwen_peer.cpp src/qwen_session_wake.cpp)
qwen_target(qwen_provider)
target_include_directories(qwen_provider PUBLIC src)
target_link_libraries(qwen_provider PUBLIC logonomic_cli_addon_sdk::sdk)

add_executable(qwen_adapter src/adapter.cpp)
target_link_libraries(qwen_adapter PRIVATE qwen_provider)
qwen_target(qwen_adapter)
add_executable(qwen_helper src/qwen_helper.cpp)
target_link_libraries(qwen_helper PRIVATE qwen_provider)
qwen_target(qwen_helper)
logonomic_cli_addon_add_launch_bridge(qwen_launch_bridge)
qwen_target(qwen_launch_bridge)

logonomic_cli_addon_package_platform(QWEN_PLATFORM)
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
logonomic_cli_addon_add_package(qwen_package logonomic.cli.qwen QWEN_PACKAGE_DIRECTORY
    ADAPTER qwen_adapter
    HELPERS qwen_helper qwen_launch_bridge
    INDEX_ARGUMENTS
        --third-party "assets/qwen_dark.svg" "notices/qwen-LICENSE.txt"
            "provenance/qwen_logo.json"
        --third-party "assets/qwen_light.svg" "notices/qwen-LICENSE.txt"
            "provenance/qwen_logo.json")

# Everything below is the package's own test suite, which only its standalone
# build configures; an embedding build stages the package and stops here.
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()

enable_testing()
add_executable(qwen_wake_tests tests/qwen_wake_tests.cpp)
target_link_libraries(qwen_wake_tests PRIVATE qwen_provider)
qwen_target(qwen_wake_tests)
add_test(NAME qwen_wake_tests COMMAND qwen_wake_tests)
set_tests_properties(qwen_wake_tests PROPERTIES TIMEOUT 60
    ENVIRONMENT "QWEN_LOGONOMIC_WAKE_BINDING=bfc0b538f38596aef4ac803b9f182cde")
add_executable(qwen_preparation_tests tests/qwen_preparation_tests.cpp)
target_link_libraries(qwen_preparation_tests PRIVATE
    qwen_provider logonomic_cli_addon_sdk::test_support)
qwen_target(qwen_preparation_tests)
add_dependencies(qwen_preparation_tests qwen_package)
add_test(NAME qwen_preparation_tests COMMAND qwen_preparation_tests)
set_tests_properties(qwen_preparation_tests PROPERTIES TIMEOUT 120)
add_executable(qwen_contract_tests tests/qwen_contract_tests.cpp)
target_link_libraries(qwen_contract_tests PRIVATE qwen_provider)
qwen_target(qwen_contract_tests)
add_dependencies(qwen_contract_tests qwen_package)
add_test(NAME qwen_contract_tests COMMAND qwen_contract_tests "${QWEN_PACKAGE_DIRECTORY}")
add_executable(qwen_usage_tests tests/qwen_usage_tests.cpp)
target_link_libraries(qwen_usage_tests PRIVATE qwen_provider)
qwen_target(qwen_usage_tests)
add_test(NAME qwen_usage_tests COMMAND qwen_usage_tests)
add_executable(qwen_acp_tests tests/qwen_acp_tests.cpp)
target_link_libraries(qwen_acp_tests PRIVATE qwen_provider)
qwen_target(qwen_acp_tests)
add_test(NAME qwen_acp_tests COMMAND qwen_acp_tests)
add_test(NAME qwen_package_validation COMMAND logonomic_cli_addon_validate "${QWEN_PACKAGE_DIRECTORY}" --platform "${QWEN_PLATFORM}")
add_test(NAME qwen_package_conformance COMMAND logonomic_cli_addon_conformance "${QWEN_PACKAGE_DIRECTORY}")
set_tests_properties(qwen_contract_tests qwen_usage_tests qwen_acp_tests qwen_package_validation qwen_package_conformance PROPERTIES TIMEOUT 120)
add_executable(qwen_usage_cache_tests tests/qwen_usage_cache_tests.cpp)
target_link_libraries(qwen_usage_cache_tests PRIVATE
    qwen_provider logonomic_cli_addon_sdk::test_support)
qwen_target(qwen_usage_cache_tests)
add_dependencies(qwen_usage_cache_tests qwen_package)
add_test(NAME qwen_usage_cache_tests
    COMMAND qwen_usage_cache_tests "${QWEN_PACKAGE_DIRECTORY}")
set_tests_properties(qwen_usage_cache_tests PROPERTIES TIMEOUT 180 LABELS "usage;cache;packaged-adapter")

add_executable(qwen_continuation_tests tests/qwen_continuation_tests.cpp)
target_link_libraries(qwen_continuation_tests PRIVATE qwen_provider)
qwen_target(qwen_continuation_tests)
add_test(NAME qwen_continuation_tests COMMAND qwen_continuation_tests)
set_tests_properties(qwen_continuation_tests PROPERTIES TIMEOUT 120 LABELS "addon;protocol;continuation")

add_executable(qwen_acp_causal_tests tests/qwen_acp_causal_tests.cpp)
target_link_libraries(qwen_acp_causal_tests PRIVATE qwen_provider)
qwen_target(qwen_acp_causal_tests)
add_test(NAME qwen_acp_causal_tests COMMAND qwen_acp_causal_tests)
set_tests_properties(qwen_acp_causal_tests PROPERTIES TIMEOUT 120 LABELS "addon;protocol;causality")

# Exercise this repository's actual producer, never a substitute packaging
# implementation. The isolated nested configure uses this same SDK source;
# it does not change the owned master dependency or edit an installed package.
option(LOGONOMIC_CLI_ADDON_RECIPE_TESTS
    "Register isolated package rebuild regressions (requires Python 3)" OFF)
if(LOGONOMIC_CLI_ADDON_RECIPE_TESTS)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    get_target_property(_membership_sdk_source logonomic_cli_addon_sdk::sdk SOURCE_DIR)
    add_test(NAME qwen_package_membership
        COMMAND "${Python3_EXECUTABLE}" "${_membership_sdk_source}/cmake/package_membership_tests.py"
            "--source=${CMAKE_CURRENT_SOURCE_DIR}" "--sdk=${_membership_sdk_source}"
            "--cmake=${CMAKE_COMMAND}" "--target=qwen_package" "--package-id=logonomic.cli.qwen"
            "--generator=${CMAKE_GENERATOR}" "--config=$<CONFIG>" "--build-type=${CMAKE_BUILD_TYPE}"
            "--generator-platform=${CMAKE_GENERATOR_PLATFORM}" "--toolset=${CMAKE_GENERATOR_TOOLSET}"
            "--toolchain=${CMAKE_TOOLCHAIN_FILE}" "--compiler=${CMAKE_CXX_COMPILER}"
            "--make-program=${CMAKE_MAKE_PROGRAM}" "--prefix-path=${CMAKE_PREFIX_PATH}"
            "--validator=$<TARGET_FILE:logonomic_cli_addon_validate>" "--platform=${QWEN_PLATFORM}")
    set_tests_properties(qwen_package_membership PROPERTIES
        TIMEOUT 1800 RESOURCE_LOCK cli_addon_recipe_rebuild LABELS "cli-addon;delivery;recipe;owning-path")
endif()
# Contract and recipe tests must not be confused with real native qualification.
get_directory_property(_provider_tests TESTS)
set_property(TEST ${_provider_tests} APPEND PROPERTY LABELS "cli-addon;provider-contract")
