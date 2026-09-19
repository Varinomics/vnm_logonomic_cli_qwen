cmake_minimum_required(VERSION 3.25)
cmake_policy(SET CMP0141 NEW)
set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
set(CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV OFF CACHE BOOL "Do not capture configure environment" FORCE)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES CMAKE_FASTBUILD_CAPTURE_SYSTEM_ENV)
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
elseif(CMAKE_GENERATOR STREQUAL "FASTBuild")
    message(FATAL_ERROR "FASTBuild package builds require direct FetchContent population (CMP0168).")
endif()
project(logonomic_cli_qwen VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(LOGONOMIC_CLI_ADDON_SDK_SOURCE "" CACHE PATH "Local Logonomic CLI add-on SDK source override")
set(LOGONOMIC_CLI_ADDON_SDK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
if(LOGONOMIC_CLI_ADDON_SDK_SOURCE)
    add_subdirectory("${LOGONOMIC_CLI_ADDON_SDK_SOURCE}" "${CMAKE_BINARY_DIR}/sdk")
else()
    include(FetchContent)
    FetchContent_Declare(logonomic_sdk_source
        GIT_REPOSITORY https://github.com/Varinomics/logonomic.git
        GIT_TAG master
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
    FetchContent_MakeAvailable(logonomic_sdk_source)
endif()

function(qwen_target target)
    target_link_libraries(${target} PRIVATE logonomic_cli_addon_sdk::sdk)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${target} PRIVATE
            _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

add_library(qwen_provider STATIC
    src/qwen_accounts.cpp src/qwen_usage.cpp src/qwen_profile.cpp
    src/qwen_acp.cpp src/qwen_config.cpp src/qwen_launcher.cpp
    src/qwen_lifecycle.cpp src/qwen_preparation.cpp)
qwen_target(qwen_provider)
target_include_directories(qwen_provider PUBLIC src)
target_link_libraries(qwen_provider PUBLIC logonomic_cli_addon_sdk::sdk)

add_executable(qwen_adapter src/adapter.cpp)
target_link_libraries(qwen_adapter PRIVATE qwen_provider)
qwen_target(qwen_adapter)
add_executable(qwen_helper src/qwen_helper.cpp)
target_link_libraries(qwen_helper PRIVATE qwen_provider)
qwen_target(qwen_helper)
add_executable(qwen_launch_bridge src/qwen_launch_bridge.cpp)
qwen_target(qwen_launch_bridge)

logonomic_cli_addon_package_platform(QWEN_PLATFORM)
logonomic_cli_addon_package_directory(logonomic.cli.qwen QWEN_PACKAGE_DIRECTORY)
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
file(GLOB_RECURSE package_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/package/assets/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance/*"
    "${CMAKE_CURRENT_SOURCE_DIR}/package/notices/*")
add_custom_command(
    OUTPUT "${QWEN_PACKAGE_DIRECTORY}/package.files.json"
    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${QWEN_PACKAGE_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${QWEN_PACKAGE_DIRECTORY}/adapter" "${QWEN_PACKAGE_DIRECTORY}/helpers"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/assets" "${QWEN_PACKAGE_DIRECTORY}/assets"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/capabilities" "${QWEN_PACKAGE_DIRECTORY}/capabilities"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/package/provenance" "${QWEN_PACKAGE_DIRECTORY}/provenance"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/package/notices" "${QWEN_PACKAGE_DIRECTORY}/notices"
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_BINARY_DIR}/generated/logonomic-cli-addon.json" "${QWEN_PACKAGE_DIRECTORY}/logonomic-cli-addon.json"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:qwen_adapter>" "${QWEN_PACKAGE_DIRECTORY}/adapter/qwen_adapter${CMAKE_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:qwen_helper>" "${QWEN_PACKAGE_DIRECTORY}/helpers/qwen_helper${CMAKE_EXECUTABLE_SUFFIX}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:qwen_launch_bridge>" "${QWEN_PACKAGE_DIRECTORY}/helpers/qwen_launch_bridge${CMAKE_EXECUTABLE_SUFFIX}"
    COMMAND "$<TARGET_FILE:logonomic_cli_addon_index>" "${QWEN_PACKAGE_DIRECTORY}"
        --third-party "assets/qwen.svg" "notices/qwen-LICENSE.txt"
            "provenance/qwen_logo.json"
        --third-party "notices/qwen-NOTICE.txt" "notices/qwen-LICENSE.txt"
            "provenance/qwen_logo.json"
    DEPENDS qwen_adapter qwen_helper qwen_launch_bridge logonomic_cli_addon_index
        "${CMAKE_BINARY_DIR}/generated/logonomic-cli-addon.json" ${package_files}
    VERBATIM)
add_custom_target(qwen_package ALL DEPENDS "${QWEN_PACKAGE_DIRECTORY}/package.files.json")

enable_testing()
add_executable(qwen_preparation_tests tests/qwen_preparation_tests.cpp)
target_link_libraries(qwen_preparation_tests PRIVATE qwen_provider)
qwen_target(qwen_preparation_tests)
add_test(NAME qwen_preparation_tests COMMAND qwen_preparation_tests)
set_tests_properties(qwen_preparation_tests PROPERTIES TIMEOUT 120)
add_executable(qwen_contract_tests tests/qwen_contract_tests.cpp)
target_link_libraries(qwen_contract_tests PRIVATE qwen_provider)
qwen_target(qwen_contract_tests)
add_test(NAME qwen_contract_tests COMMAND qwen_contract_tests)
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
logonomic_cli_addon_install_package("${QWEN_PACKAGE_DIRECTORY}" logonomic.cli.qwen)
