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
            GIT_REPOSITORY https://github.com/Varinomics/vnm_logonomic_cli_addon.git
            GIT_TAG master
            GIT_SHALLOW TRUE
            SOURCE_SUBDIR sdk/logonomic_cli_addon_sdk)
        FetchContent_MakeAvailable(logonomic_sdk_source)
    endif()
endif()

include(cmake/qwen_observation_dependency.cmake)
logonomic_cli_addon_add_filesystem_observer()

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
    src/qwen_conversation.cpp src/qwen_resume.cpp
    src/qwen_accounts.cpp src/qwen_usage.cpp src/qwen_profile.cpp
    src/qwen_acp.cpp src/qwen_authoring.cpp src/qwen_config.cpp src/qwen_launcher.cpp src/qwen_launcher_approval.cpp
    src/qwen_lifecycle.cpp src/qwen_observer.cpp src/qwen_preparation.cpp
    src/qwen_peer.cpp src/qwen_session_wake.cpp)
qwen_target(qwen_provider)
target_include_directories(qwen_provider PUBLIC src)
target_link_libraries(qwen_provider PUBLIC logonomic_cli_addon_sdk::sdk
    logonomic_cli_addon_sdk::filesystem_observer)

add_executable(qwen_adapter src/adapter.cpp)
target_compile_definitions(qwen_adapter PRIVATE LOGONOMIC_CLI_ADDON_ADAPTER_VERSION="${PROJECT_VERSION}")
target_link_libraries(qwen_adapter PRIVATE qwen_provider)
qwen_target(qwen_adapter)
add_executable(qwen_helper src/qwen_helper.cpp)
target_link_libraries(qwen_helper PRIVATE qwen_provider)
qwen_target(qwen_helper)
logonomic_cli_addon_add_launch_bridge(qwen_launch_bridge)
qwen_target(qwen_launch_bridge)

logonomic_cli_addon_package_platform(QWEN_PLATFORM)
configure_file(package/logonomic-cli-addon.json.in generated/logonomic-cli-addon.json @ONLY)
set(QWEN_QTBASE_SOURCE "" CACHE PATH "qtbase source matching the selected Qt runtime (notices)")
set(QWEN_RUNTIME_NOTICES_DIRECTORY "" CACHE PATH
    "Copyright/license files for external runtimes, named <library filename>.txt")
set(QWEN_LINUX_RUNTIME_MATERIAL "" CACHE FILEPATH
    "Exact prebuilt input provenance from the selected Qt/runtime acquisition")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(QWEN_PATCHELF_EXECUTABLE patchelf REQUIRED)
endif()
logonomic_cli_addon_add_qt_package(qwen_package logonomic.cli.qwen QWEN_PACKAGE_DIRECTORY
    ADAPTER qwen_adapter
    HELPERS qwen_helper qwen_launch_bridge
    QTBASE_SOURCE "${QWEN_QTBASE_SOURCE}"
    RUNTIME_NOTICES "${QWEN_RUNTIME_NOTICES_DIRECTORY}"
    LINUX_RUNTIME_MATERIAL "${QWEN_LINUX_RUNTIME_MATERIAL}"
    PATCHELF "${QWEN_PATCHELF_EXECUTABLE}"
    EXTRA_FILES LICENSE notices/LICENSE
    OUT_RUNTIME_CHECK QWEN_RUNTIME_CHECK
    OUT_QTBASE_SOURCE QWEN_QTBASE_SOURCE
    INDEX_ARGUMENTS
        --third-party assets/qwen_dark.svg notices/qwen-LICENSE.txt provenance/qwen_logo.json
        --third-party assets/qwen_light.svg notices/qwen-LICENSE.txt provenance/qwen_logo.json)

# Everything below is the package's own test suite, which only its standalone
# build configures; an embedding build stages the package and stops here.
if(LOGONOMIC_CLI_ADDON_EMBEDDED)
    return()
endif()

enable_testing()
add_executable(qwen_observer_tests tests/qwen_observer_tests.cpp)
target_link_libraries(qwen_observer_tests PRIVATE qwen_provider logonomic_cli_addon_sdk::test_support)
qwen_target(qwen_observer_tests)
add_test(NAME qwen_observer_tests COMMAND qwen_observer_tests)
set_tests_properties(qwen_observer_tests PROPERTIES TIMEOUT 120)
add_executable(qwen_conversation_tests tests/qwen_conversation_tests.cpp)
target_link_libraries(qwen_conversation_tests PRIVATE qwen_provider)
qwen_target(qwen_conversation_tests)
if(WIN32)
    target_link_libraries(qwen_conversation_tests PRIVATE advapi32)
endif()
add_test(NAME qwen_conversation_tests COMMAND qwen_conversation_tests)
set_tests_properties(qwen_conversation_tests PROPERTIES TIMEOUT 30)

add_executable(qwen_resume_tests tests/qwen_resume_tests.cpp)
target_link_libraries(qwen_resume_tests PRIVATE qwen_provider)
qwen_target(qwen_resume_tests)
add_dependencies(qwen_resume_tests qwen_package)
add_test(NAME qwen_resume_tests COMMAND qwen_resume_tests "${QWEN_PACKAGE_DIRECTORY}")
set_tests_properties(qwen_resume_tests PROPERTIES TIMEOUT 120)

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
add_executable(qwen_initial_prompt_tests tests/qwen_initial_prompt_tests.cpp)
target_link_libraries(qwen_initial_prompt_tests PRIVATE
    qwen_provider logonomic_cli_addon_sdk::test_support)
qwen_target(qwen_initial_prompt_tests)
if(MINGW)
    target_link_options(qwen_initial_prompt_tests PRIVATE -municode)
endif()
add_test(NAME qwen_initial_prompt_tests COMMAND qwen_initial_prompt_tests)
set_tests_properties(qwen_initial_prompt_tests PROPERTIES TIMEOUT 180 LABELS "addon;initial-prompt;native-argv")
add_executable(qwen_contract_tests tests/qwen_contract_tests.cpp)
target_link_libraries(qwen_contract_tests PRIVATE qwen_provider)
qwen_target(qwen_contract_tests)
add_dependencies(qwen_contract_tests qwen_package)
add_test(NAME qwen_contract_tests COMMAND qwen_contract_tests "${QWEN_PACKAGE_DIRECTORY}")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
get_target_property(_qwen_sdk_source logonomic_cli_addon_sdk::sdk SOURCE_DIR)
add_test(NAME qwen_helper_tests
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/qwen_helper_tests.py"
        "${QWEN_PACKAGE_DIRECTORY}" "$<TARGET_FILE:qwen_contract_tests>")
set_tests_properties(qwen_helper_tests PROPERTIES TIMEOUT 60)
add_executable(qwen_usage_tests tests/qwen_usage_tests.cpp)
target_link_libraries(qwen_usage_tests PRIVATE qwen_provider)
qwen_target(qwen_usage_tests)
add_test(NAME qwen_usage_tests COMMAND qwen_usage_tests)
add_executable(qwen_usage_page_tests tests/qwen_usage_page_tests.cpp)
target_link_libraries(qwen_usage_page_tests PRIVATE qwen_provider)
qwen_target(qwen_usage_page_tests)
add_test(NAME qwen_usage_page_tests COMMAND qwen_usage_page_tests)
add_executable(qwen_acp_tests tests/qwen_acp_tests.cpp)
target_link_libraries(qwen_acp_tests PRIVATE qwen_provider)
qwen_target(qwen_acp_tests)
add_test(NAME qwen_acp_tests COMMAND qwen_acp_tests)
add_test(NAME qwen_package_validation COMMAND logonomic_cli_addon_validate "${QWEN_PACKAGE_DIRECTORY}" --platform "${QWEN_PLATFORM}")
add_test(NAME qwen_package_conformance COMMAND logonomic_cli_addon_conformance "${QWEN_PACKAGE_DIRECTORY}")
set_tests_properties(qwen_contract_tests qwen_usage_tests qwen_usage_page_tests qwen_acp_tests qwen_package_validation qwen_package_conformance PROPERTIES TIMEOUT 120)
add_test(NAME qwen_dependency_preparation_tests
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/.github/scripts/test_prepare_ci.py"
        --sdk "${_qwen_sdk_source}")
add_test(NAME qwen_package_runtime_layout
    COMMAND "${CMAKE_COMMAND}" "-DPACKAGE=${QWEN_PACKAGE_DIRECTORY}"
        "-DEXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_runtime_layout_tests.cmake")
add_test(NAME qwen_package_runtime_closure COMMAND ${QWEN_RUNTIME_CHECK})
set_tests_properties(qwen_package_runtime_closure PROPERTIES TIMEOUT 120)
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
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_membership_tests.py"
            "--source=${CMAKE_CURRENT_SOURCE_DIR}" "--sdk=${_membership_sdk_source}"
            "--observation-source=${qwen_observation_source}"
            "--dispatch-source=${qwen_dispatch_source}"
            "--vnm-cmake-source=${qwen_vnm_cmake_source}"
            "--observation-prefix=${vnm_filesystem_observation_DIR}"
            "--dispatch-prefix=${vnm_qt_dispatch_DIR}"
            "--qtbase-source=${QWEN_QTBASE_SOURCE}"
            "--runtime-notices=${QWEN_RUNTIME_NOTICES_DIRECTORY}"
            "--linux-runtime-material=${QWEN_LINUX_RUNTIME_MATERIAL}"
            "--patchelf=${QWEN_PATCHELF_EXECUTABLE}"
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
if(WIN32)
    add_executable(qwen_native_path_tests tests/qwen_native_path_tests.cpp)
    target_link_libraries(qwen_native_path_tests PRIVATE
        qwen_provider logonomic_cli_addon_sdk::test_support)
    qwen_target(qwen_native_path_tests)
    add_dependencies(qwen_native_path_tests qwen_package)
    add_test(NAME qwen_native_path_tests COMMAND qwen_native_path_tests "${QWEN_PACKAGE_DIRECTORY}")
    set_tests_properties(qwen_native_path_tests PROPERTIES TIMEOUT 120 LABELS "usage;filesystem;packaged-adapter")
    add_test(NAME qwen_native_account_paths COMMAND qwen_native_path_tests "${QWEN_PACKAGE_DIRECTORY}" --account-paths)
    set_tests_properties(qwen_native_account_paths PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "accounts;filesystem;packaged-adapter")
endif()
get_directory_property(_provider_tests TESTS)
set_property(TEST ${_provider_tests} APPEND PROPERTY LABELS "cli-addon;provider-contract")

add_executable(qwen_error_boundary_tests tests/qwen_error_boundary_tests.cpp)
target_compile_definitions(qwen_error_boundary_tests PRIVATE LOGONOMIC_CLI_ADDON_ADAPTER_VERSION="${PROJECT_VERSION}")
target_link_libraries(qwen_error_boundary_tests PRIVATE qwen_provider logonomic_cli_addon_sdk::test_support)
qwen_target(qwen_error_boundary_tests)
add_test(NAME qwen_error_boundary_tests COMMAND qwen_error_boundary_tests)

set(QWEN_NATIVE_TEST_COMMAND "" CACHE STRING "Installed Qwen command and arguments for opt-in native acceptance tests")
if(QWEN_NATIVE_TEST_COMMAND)
    add_dependencies(qwen_contract_tests logonomic_cli_addon_host_simulator)
    add_test(NAME qwen_live_switch_native_tests
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/qwen_live_switch_native_tests.py"
            "${QWEN_PACKAGE_DIRECTORY}" "$<TARGET_FILE:logonomic_cli_addon_host_simulator>"
            "$<TARGET_FILE:qwen_contract_tests>" -- ${QWEN_NATIVE_TEST_COMMAND})
    set_tests_properties(qwen_live_switch_native_tests PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "cli-addon;native-acceptance;live-switch")
    add_test(NAME qwen_acp_native_tests
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/qwen_acp_native_tests.py"
            "${QWEN_PACKAGE_DIRECTORY}" "$<TARGET_FILE:qwen_contract_tests>"
            -- ${QWEN_NATIVE_TEST_COMMAND})
    add_test(NAME qwen_acp_textless_intermediate_native_tests
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/qwen_acp_native_tests.py"
            --textless-intermediate "${QWEN_PACKAGE_DIRECTORY}" "$<TARGET_FILE:qwen_contract_tests>"
            -- ${QWEN_NATIVE_TEST_COMMAND})
    set_tests_properties(qwen_acp_native_tests qwen_acp_textless_intermediate_native_tests PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "cli-addon;native-acceptance;acp")
endif()

if(WIN32)
    # Tests execute build-tree programs before package deployment supplies their
    # app-local DLLs. Use the selected kit for these executable paths only.
    get_directory_property(_qwen_build_tree_tests TESTS)
    set_tests_properties(${_qwen_build_tree_tests} PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
endif()
