# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# The processor name is collected when running the executable. Recording it
# at build time would identify the wrong processor when the executable is copied.
foreach(required IN ITEMS EXECUTABLE OUTPUT)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Supply a nonempty \"-D${required}=...\" argument before -P Run.cmake")
    endif()
endforeach()
get_filename_component(EXECUTABLE "${EXECUTABLE}" ABSOLUTE)
get_filename_component(OUTPUT "${OUTPUT}" ABSOLUTE)
if(NOT DEFINED PROCESSOR)
    if(CMAKE_HOST_WIN32)
        cmake_host_system_information(RESULT PROCESSOR QUERY WINDOWS_REGISTRY
            "HKLM/HARDWARE/DESCRIPTION/System/CentralProcessor/0" VALUE ProcessorNameString)
    elseif(CMAKE_HOST_APPLE)
        execute_process(COMMAND sysctl -n machdep.cpu.brand_string
            OUTPUT_VARIABLE PROCESSOR OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY)
    else()
        cmake_host_system_information(RESULT PROCESSOR QUERY PROCESSOR_NAME)
    endif()
endif()
string(STRIP "${PROCESSOR}" PROCESSOR)
if(PROCESSOR STREQUAL "")
    set(PROCESSOR "unavailable")
endif()
message(STATUS "Processor: ${PROCESSOR}")
set(arguments --processor "${PROCESSOR}")
if(DEFINED SEED)
    list(APPEND arguments --seed "${SEED}")
endif()
if(RETRY_ONLY)
    list(APPEND arguments --retry-only)
endif()
execute_process(COMMAND "${EXECUTABLE}" ${arguments}
    OUTPUT_FILE "${OUTPUT}"
    RESULT_VARIABLE result
)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Benchmark execution failed: ${EXECUTABLE}\n${result}")
endif()
message(STATUS "Results: ${OUTPUT}")
