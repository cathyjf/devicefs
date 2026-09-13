# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

set(benchmark_source "${CMAKE_CURRENT_LIST_DIR}")
set(benchmark_output "${CMAKE_CURRENT_BINARY_DIR}/transcoding-benchmark")
set(benchmark_simdutf_directory "${benchmark_output}/generated/simdutf")
list(APPEND SIMDUTF_ANALYSIS_EXCLUDE_DIRECTORIES "${benchmark_simdutf_directory}")
add_custom_command(
    OUTPUT
        "${benchmark_output}/sizing.ixx"
        "${benchmark_simdutf_directory}/combined.h"
    COMMAND "${CMAKE_COMMAND}"
        "-DTERMINAL_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUTPUT_DIRECTORY=${benchmark_output}"
        -P "${benchmark_source}/Generate.cmake"
    DEPENDS
        transcoding.ixx
        dependencies/simdutf/simdutf.h
        dependencies/simdutf/simdutf.cpp
        compat/simdutf_preamble.h
        cmake/GenerateSimdutf.cmake
        "${benchmark_source}/Generate.cmake"
        "${benchmark_source}/exports.cpp.in"
    VERBATIM
)
add_library(transcoding-benchmark-policies ${DEVICEFS_TERMINAL_LIBRARY_TYPE})
target_sources(transcoding-benchmark-policies PUBLIC
    FILE_SET CXX_MODULES BASE_DIRS "${benchmark_output}"
    FILES "${benchmark_output}/sizing.ixx"
)
target_include_directories(transcoding-benchmark-policies PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
)
target_include_directories(transcoding-benchmark-policies SYSTEM PRIVATE
    "${benchmark_output}/generated"
)

add_executable(devicefs-transcoding-benchmark "${benchmark_source}/main.cpp")
target_sources(devicefs-transcoding-benchmark PRIVATE
    FILE_SET CXX_MODULES FILES scope_exit.ixx
)
option(DEVICEFS_TRANSCODING_BENCHMARK_DEFER_DEALLOCATION
    "Defer deallocations during batches" OFF)
if(DEVICEFS_TRANSCODING_BENCHMARK_DEFER_DEALLOCATION)
    target_compile_definitions(devicefs-transcoding-benchmark PRIVATE BENCHMARK_DEFER_DEALLOCATION)
endif()
target_link_libraries(devicefs-transcoding-benchmark PRIVATE
    transcoding-benchmark-policies
)
if(MSVC)
    set(benchmark_architecture "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}")
else()
    set(benchmark_architecture "${CMAKE_SYSTEM_PROCESSOR}")
endif()
target_compile_definitions(devicefs-transcoding-benchmark PRIVATE
    "BENCHMARK_COMPILER=\"${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}\""
    "BENCHMARK_ARCHITECTURE=\"${benchmark_architecture}\""
    "BENCHMARK_CONFIGURATION=\"$<CONFIG>\""
)
list(APPEND DEVICEFS_TERMINAL_ANALYSIS_TARGETS
    transcoding-benchmark-policies
    devicefs-transcoding-benchmark
)
