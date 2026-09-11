# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 4.2)

project(devicefs-terminal LANGUAGES CXX)

if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    unset(CMAKE_CXX_STDLIB_MODULES_JSON)
    find_file(CMAKE_CXX_STDLIB_MODULES_JSON NAMES libc++.modules.json
        HINTS
            "${LLVM_BIN_DIRECTORY}/../lib/c++"
            "${LLVM_BIN_DIRECTORY}/../lib"
        REQUIRED
    )
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
if(WIN32)
    find_program(POWERSHELL_EXECUTABLE NAMES pwsh REQUIRED)
    set(CMAKE_VS_GLOBALS
        "UseMultiToolTask=true"
        "EnableClServerMode=true"
        "BuildPassReferences=true"
    )
endif()

option(DEVICEFS_TERMINAL_BUILD_TESTS "Build the terminal test entry point" OFF)

get_filename_component(DEVICEFS_ROOT "../.." ABSOLUTE)
set(WIL_INCLUDE_DIR "${DEVICEFS_ROOT}/dependencies/wil/include")

if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    include("${DEVICEFS_ROOT}/misc/MsvcChecks.cmake")
    set(DEVICEFS_TERMINAL_LIBRARY_TYPE STATIC)
else()
    set(DEVICEFS_TERMINAL_LIBRARY_TYPE OBJECT)
endif()

add_library(windows-terminal-width ${DEVICEFS_TERMINAL_LIBRARY_TYPE}
    dependencies/terminal/src/types/CodepointWidthDetector.cpp
)
target_include_directories(windows-terminal-width PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_CURRENT_SOURCE_DIR}/dependencies"
)

set(SIMDUTF_GENERATED_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/simdutf/combined.h")
add_custom_command(
    OUTPUT "${SIMDUTF_GENERATED_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/dependencies/simdutf"
        "-DOUTPUT=${SIMDUTF_GENERATED_HEADER}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateSimdutf.cmake"
    DEPENDS
        dependencies/simdutf/simdutf.h
        dependencies/simdutf/simdutf.cpp
        compat/simdutf_preamble.h
        cmake/GenerateSimdutf.cmake
    VERBATIM
)
add_custom_target(generate-simdutf DEPENDS "${SIMDUTF_GENERATED_HEADER}")

add_library(devicefs-terminal ${DEVICEFS_TERMINAL_LIBRARY_TYPE})
add_dependencies(devicefs-terminal generate-simdutf)
target_include_directories(devicefs-terminal SYSTEM PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_sources(devicefs-terminal PUBLIC
    FILE_SET CXX_MODULES FILES
        base_console.ixx
        drawing.ixx
        formatting.ixx
        frame.ixx
        layout.ixx
        menu.ixx
        safecast.ixx
        scope_exit.ixx
        terminal_reports.ixx
        terminal.ixx
        transcoding.ixx
        text.ixx
        vt.ixx
)
if(WIN32)
    target_sources(devicefs-terminal PUBLIC FILE_SET CXX_MODULES FILES windows.ixx)
else()
    target_sources(devicefs-terminal PUBLIC FILE_SET CXX_MODULES FILES unix.ixx)
endif()
target_link_libraries(devicefs-terminal PRIVATE windows-terminal-width)
target_include_directories(devicefs-terminal PRIVATE
    "${DEVICEFS_ROOT}/src/modules/include"
)

set(DEVICEFS_TERMINAL_ANALYSIS_TARGETS
    devicefs-terminal
    windows-terminal-width
)
if(DEVICEFS_TERMINAL_BUILD_TESTS)
    add_executable(devicefs-terminal-test
        tests/main.cpp
    )
    if(WIN32)
        target_sources(devicefs-terminal-test PRIVATE
            "${DEVICEFS_ROOT}/src/resources/devicefs.manifest")
    endif()
    target_sources(devicefs-terminal-test PRIVATE
        FILE_SET CXX_MODULES FILES tests/menu_tests.ixx tests/frame_tests.ixx
            tests/test_support.ixx tests/native_tests.ixx tests/text_tests.ixx
            tests/menu_measurements.ixx tests/transcoding_tests.ixx
    )
    target_link_libraries(devicefs-terminal-test PRIVATE
        devicefs-terminal
        windows-terminal-width
    )
    target_include_directories(devicefs-terminal-test PRIVATE
        "${DEVICEFS_ROOT}/src/modules/include"
    )
    if(WIN32)
        target_link_options(devicefs-terminal-test PRIVATE
            /GUARD:CF /guard:ehcont
            $<$<STREQUAL:${CMAKE_CXX_COMPILER_ARCHITECTURE_ID},x64>:/CETCOMPAT>
        )
    endif()
    list(APPEND DEVICEFS_TERMINAL_ANALYSIS_TARGETS devicefs-terminal-test)

    enable_testing()
    add_test(NAME devicefs_terminal
        COMMAND devicefs-terminal-test --self-test
    )
    if(WIN32)
        add_test(NAME devicefs_terminal_native
            COMMAND "${POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
                -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/native_windows.ps1"
                -Executable "$<TARGET_FILE:devicefs-terminal-test>"
        )
    else()
        target_link_libraries(devicefs-terminal-test PRIVATE util)
        add_test(NAME devicefs_terminal_native COMMAND devicefs-terminal-test --native-test)
    endif()
    set_tests_properties(devicefs_terminal devicefs_terminal_native PROPERTIES TIMEOUT 30)
endif()

foreach(target IN LISTS DEVICEFS_TERMINAL_ANALYSIS_TARGETS)
    if(NOT WIN32)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
        continue()
    endif()
    set_target_properties(${target} PROPERTIES
        VS_GLOBAL_EnableCppCoreCheck true
        VS_GLOBAL_RunCodeAnalysis true
        VS_GLOBAL_CodeAnalysisRuleSet "${DEVICEFS_ROOT}/.ruleset"
        VS_GLOBAL_CAExcludePath "${WIL_INCLUDE_DIR};$(CAExcludePath)"
    )
    target_include_directories(${target} SYSTEM PRIVATE "${WIL_INCLUDE_DIR}")
    target_compile_definitions(${target} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _MSVC_STL_HARDENING=1
    )
    target_compile_options(${target} PRIVATE
        /MP /W4 /WX /permissive- /EHsc /utf-8 /guard:cf /guard:ehcont ${DEVICEFS_MSVC_CHECKS}
        /analyze:external- "SHELL:/analyze:log $(IntDir)"
        /analyze:log:format:sarif /analyze:sarif:configuration
        /analyze:log:compilerwarnings
    )
endforeach()

if(WIN32)
    add_custom_target(check-terminal-code-analysis ALL
        COMMAND "${POWERSHELL_EXECUTABLE}"
            -NoProfile -NonInteractive -File
            "${DEVICEFS_ROOT}/misc/Check-NativeCodeAnalysis.ps1"
            -BuildDirectory "${CMAKE_CURRENT_BINARY_DIR}"
            -Configuration "$<CONFIG>"
        USES_TERMINAL
        VERBATIM
    )
    add_dependencies(check-terminal-code-analysis ${DEVICEFS_TERMINAL_ANALYSIS_TARGETS})
endif()
