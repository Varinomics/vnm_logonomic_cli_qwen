# The leaf requires Core only. Locate notices from the selected Qt source,
# including copyrights and bundled third-party license texts.
get_target_property(qwen_qmake Qt6::qmake IMPORTED_LOCATION)
if(NOT qwen_qmake)
    get_target_property(qwen_qmake_configs Qt6::qmake IMPORTED_CONFIGURATIONS)
    list(GET qwen_qmake_configs 0 qwen_qmake_config)
    get_target_property(qwen_qmake Qt6::qmake "IMPORTED_LOCATION_${qwen_qmake_config}")
endif()
execute_process(COMMAND "${qwen_qmake}" -query QT_INSTALL_PREFIX
    OUTPUT_VARIABLE qwen_qt_prefix OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
set(QWEN_QTBASE_SOURCE "" CACHE PATH "qtbase source matching the selected Qt runtime (notices)")
if(NOT QWEN_QTBASE_SOURCE AND EXISTS "${qwen_qt_prefix}/../Src/qtbase/LICENSES")
    get_filename_component(QWEN_QTBASE_SOURCE "${qwen_qt_prefix}/../Src/qtbase" ABSOLUTE)
endif()
if(NOT QWEN_QTBASE_SOURCE)
    # This tag identifies license material for the runtime already selected by
    # find_package; it is not a ceiling or a pin on supported Qt releases.
    include(FetchContent)
    FetchContent_Declare(qwen_qt_notices
        GIT_REPOSITORY https://code.qt.io/qt/qtbase.git
        GIT_TAG "v${Qt6Core_VERSION}"
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR _qwen_notices_only)
    FetchContent_MakeAvailable(qwen_qt_notices)
    set(QWEN_QTBASE_SOURCE "${qwen_qt_notices_SOURCE_DIR}")
endif()
if(NOT EXISTS "${QWEN_QTBASE_SOURCE}/LICENSES/LGPL-3.0-only.txt")
    message(FATAL_ERROR "Set QWEN_QTBASE_SOURCE to the selected Qt's qtbase source; runtime notices are required.")
endif()
file(STRINGS "${QWEN_QTBASE_SOURCE}/.cmake.conf" qwen_qt_source_version
    REGEX [=[^set\(QT_REPO_MODULE_VERSION ]=])
if(NOT qwen_qt_source_version MATCHES "\"${Qt6Core_VERSION}\"")
    message(FATAL_ERROR "QWEN_QTBASE_SOURCE must describe the selected Qt ${Qt6Core_VERSION} runtime.")
endif()
set(QWEN_RUNTIME_NOTICES_DIRECTORY "" CACHE PATH
    "Copyright/license files for external runtimes, named <library filename>.txt")
file(GLOB_RECURSE qwen_qt_notice_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
    "${QWEN_QTBASE_SOURCE}/LICENSES/*.txt"
    "${QWEN_QTBASE_SOURCE}/src/3rdparty/*LICENSE*"
    "${QWEN_QTBASE_SOURCE}/src/3rdparty/*LICENCE*"
    "${QWEN_QTBASE_SOURCE}/src/3rdparty/*COPYING*"
    "${QWEN_QTBASE_SOURCE}/src/3rdparty/*COPYRIGHT*"
    "${QWEN_QTBASE_SOURCE}/src/3rdparty/*qt_attribution*.json")
file(GLOB qwen_qt_sbom_inputs "${qwen_qt_prefix}/sbom/qtbase-*.spdx")
list(APPEND qwen_qt_notice_inputs ${qwen_qt_sbom_inputs})
set(qwen_runtime_search_dirs "$<TARGET_FILE_DIR:Qt6::Core>")
set(qwen_compiler_runtime_files "")
set(qwen_debug_runtime_files "")
if(MSVC)
    # This module resolves redistributables from the active compiler, not PATH.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    set(qwen_compiler_runtime_files "${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}")
    function(qwen_find_debug_runtime_files output)
        # Debug CRT and UCRT are private debugging/testing inputs, not release
        # redistributables. Keep their discovery settings local to this call.
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS "")
        set(CMAKE_INSTALL_DEBUG_LIBRARIES TRUE)
        set(CMAKE_INSTALL_DEBUG_LIBRARIES_ONLY TRUE)
        set(CMAKE_INSTALL_UCRT_LIBRARIES TRUE)
        include(InstallRequiredSystemLibraries)
        set(${output} "${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}" PARENT_SCOPE)
    endfunction()
    qwen_find_debug_runtime_files(qwen_debug_runtime_files)
    foreach(runtime IN LISTS qwen_compiler_runtime_files qwen_debug_runtime_files)
        get_filename_component(runtime_dir "${runtime}" DIRECTORY)
        list(APPEND qwen_runtime_search_dirs "${runtime_dir}")
    endforeach()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    logonomic_cli_addon_linux_runtime_deployer(QWEN_LINUX_RUNTIME_DEPLOYER)
    set(QWEN_LINUX_RUNTIME_MATERIAL "" CACHE FILEPATH
        "Exact prebuilt input provenance from the selected Qt/ICU acquisition")
    find_program(QWEN_PATCHELF_EXECUTABLE patchelf REQUIRED)
    foreach(target qwen_adapter qwen_helper qwen_launch_bridge)
        # Keep the build tree runnable and give the package an exactly known
        # rpath to replace. DT_RPATH resolves transitive dependencies too.
        set_target_properties(${target} PROPERTIES
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "$ORIGIN;$<TARGET_FILE_DIR:Qt6::Core>"
            INSTALL_RPATH_USE_LINK_PATH FALSE)
        target_link_options(${target} PRIVATE "LINKER:--disable-new-dtags")
        target_compile_options(${target} PRIVATE "$<$<CONFIG:Release>:-g0>")
        target_link_options(${target} PRIVATE "$<$<CONFIG:Release>:LINKER:--strip-all>")
    endforeach()
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
    logonomic_cli_addon_macos_runtime_deployer(QWEN_MACOS_RUNTIME_DEPLOYER)
    foreach(target qwen_adapter qwen_helper qwen_launch_bridge)
        target_link_options(${target} PRIVATE "LINKER:-headerpad_max_install_names")
    endforeach()
endif()
function(qwen_configure_package_assembler)
    # Cache overrides can retain native Windows separators. Once substituted
    # into quoted CMake source, those separators become escape sequences.
    # Normalize every substituted filesystem path/list in this local scope;
    # preserve list separators and the caller's selected cache/toolchain values.
    if(WIN32)
        foreach(path_variable IN ITEMS
                QWEN_PACKAGE_DIRECTORY CMAKE_CURRENT_SOURCE_DIR CMAKE_CURRENT_BINARY_DIR
                CMAKE_CXX_COMPILER CMAKE_OBJDUMP CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND
                qwen_runtime_search_dirs qwen_compiler_runtime_files qwen_debug_runtime_files
                qwen_qt_notice_inputs
                QWEN_QTBASE_SOURCE QWEN_RUNTIME_NOTICES_DIRECTORY
                QWEN_PATCHELF_EXECUTABLE)
            string(REPLACE "\\" "/" ${path_variable} "${${path_variable}}")
        endforeach()
    endif()
    configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/assemble_qwen_package.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/assemble-package.cmake.in" @ONLY)
    if(MSVC)
        configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_qwen_runtime_closure.cmake.in"
            "${CMAKE_CURRENT_BINARY_DIR}/check-runtime-closure.cmake.in" @ONLY)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/check_qwen_macos_runtime_closure.cmake.in"
            "${CMAKE_CURRENT_BINARY_DIR}/check-runtime-closure.cmake.in" @ONLY)
    endif()
endfunction()
qwen_configure_package_assembler()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/assemble-package-$<CONFIG>.cmake"
    INPUT "${CMAKE_CURRENT_BINARY_DIR}/assemble-package.cmake.in")
if(MSVC OR CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/check-runtime-closure-$<CONFIG>.cmake"
        INPUT "${CMAKE_CURRENT_BINARY_DIR}/check-runtime-closure.cmake.in")
endif()
