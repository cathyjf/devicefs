# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 4.2)

project(devicefs-terminal LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR "devicefs-terminal requires the Microsoft Visual C++ compiler on Windows")
endif()

if(MSVC_VERSION LESS 1951)
    message(FATAL_ERROR "devicefs-terminal requires MSVC Build Tools 14.51 or newer")
endif()

if(NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "^(x64|ARM64)$")
    message(FATAL_ERROR "devicefs-terminal requires an x64 or ARM64 target")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
set(CMAKE_VS_GLOBALS
    "UseMultiToolTask=true"
    "EnableClServerMode=true"
    "BuildPassReferences=true"
)

option(DEVICEFS_TERMINAL_BUILD_TESTS "Build the terminal test entry point" OFF)

get_filename_component(DEVICEFS_ROOT "../.." ABSOLUTE)
set(WIL_INCLUDE_DIR "${DEVICEFS_ROOT}/dependencies/wil/include")

add_library(windows-terminal-width STATIC
    dependencies/terminal/src/types/CodepointWidthDetector.cpp
)
target_include_directories(windows-terminal-width PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/dependencies"
)
target_include_directories(windows-terminal-width PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
)

add_library(devicefs-terminal STATIC)
target_sources(devicefs-terminal PUBLIC
    FILE_SET CXX_MODULES FILES terminal.ixx text.ixx frame.ixx menu.ixx windows.ixx
)
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
        main.cpp
        "${DEVICEFS_ROOT}/src/resources/devicefs.manifest"
    )
    target_sources(devicefs-terminal-test PRIVATE
        FILE_SET CXX_MODULES FILES menu_tests.ixx frame_tests.ixx
    )
    target_link_libraries(devicefs-terminal-test PRIVATE devicefs-terminal)
    target_include_directories(devicefs-terminal-test PRIVATE
        "${DEVICEFS_ROOT}/src/modules/include"
    )
    target_link_options(devicefs-terminal-test PRIVATE
        /GUARD:CF /guard:ehcont
        $<$<STREQUAL:${CMAKE_CXX_COMPILER_ARCHITECTURE_ID},x64>:/CETCOMPAT>
    )
    list(APPEND DEVICEFS_TERMINAL_ANALYSIS_TARGETS devicefs-terminal-test)
endif()

foreach(target IN LISTS DEVICEFS_TERMINAL_ANALYSIS_TARGETS)
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
        /MP /W4 /WX /permissive- /EHsc /utf-8 /guard:cf /guard:ehcont /sdl
        /analyze:external- "SHELL:/analyze:log $(IntDir)"
        /analyze:log:format:sarif /analyze:sarif:configuration
        /analyze:log:compilerwarnings
    )
endforeach()

find_program(POWERSHELL_EXECUTABLE NAMES pwsh REQUIRED)
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
