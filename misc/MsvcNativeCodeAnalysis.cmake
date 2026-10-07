# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

include_guard(GLOBAL)

# Configure warnings, conformance, and native analysis for a target. The
# remaining arguments are header directories to exclude from analysis.
function(devicefs_enable_native_code_analysis target)
    set_target_properties(${target} PROPERTIES
        VS_GLOBAL_EnableCppCoreCheck true
        VS_GLOBAL_RunCodeAnalysis true
        VS_GLOBAL_CodeAnalysisRuleSet
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/MsvcNativeCodeAnalysis.ruleset"
        VS_GLOBAL_CAExcludePath "${ARGN};$(CAExcludePath)"
    )
    target_compile_options(${target} PRIVATE
        /W4 /WX /permissive-
        /analyze:external- "SHELL:/analyze:log $(IntDir)"
        /analyze:log:format:sarif /analyze:sarif:configuration
        /analyze:log:compilerwarnings
    )
endfunction()

function(devicefs_add_analysis_dependencies directory)
    get_property(targets DIRECTORY "${directory}"
        PROPERTY BUILDSYSTEM_TARGETS)
    list(REMOVE_ITEM targets check-native-code-analysis)
    add_dependencies(check-native-code-analysis ${targets})

    get_property(subdirectories DIRECTORY "${directory}"
        PROPERTY SUBDIRECTORIES)
    foreach(subdirectory IN LISTS subdirectories)
        devicefs_add_analysis_dependencies("${subdirectory}")
    endforeach()
endfunction()

# Call after declaring all targets and subdirectories. The checker reads reports
# throughout the build directory, so it must wait for targets in every directory.
function(devicefs_add_native_code_analysis_check)
    if(NOT WIN32 OR NOT PROJECT_IS_TOP_LEVEL)
        return()
    endif()

    add_custom_target(check-native-code-analysis ALL
        COMMAND "${POWERSHELL_EXECUTABLE}"
            -NoProfile -NonInteractive -File
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/Check-NativeCodeAnalysis.ps1"
            -BuildDirectory "${CMAKE_CURRENT_BINARY_DIR}"
            -Configuration "$<CONFIG>"
        USES_TERMINAL
        VERBATIM
    )
    devicefs_add_analysis_dependencies("${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()
