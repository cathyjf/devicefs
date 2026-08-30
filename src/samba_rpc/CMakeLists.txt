# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 4.4)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_LIST_DIR}/cmake/llvm.cmake"
        CACHE FILEPATH "DeviceFs Samba RPC LLVM toolchain"
    )
endif()

set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD
    "f35a9ac6-8463-4d38-8eec-5d6008153e7d"
)

project(devicefs-samba-rpc LANGUAGES C CXX)

find_program(PIDL_EXECUTABLE NAMES pidl
    DOC "Samba PIDL compiler"
)
if(NOT PIDL_EXECUTABLE)
    message(FATAL_ERROR
        "Samba PIDL was not found; set PIDL_EXECUTABLE to Samba's pidl program"
    )
endif()
find_path(SAMBA_INCLUDE_DIRECTORY dcesrv_core.h
    PATH_SUFFIXES samba-4.0
    REQUIRED
)
find_path(TALLOC_INCLUDE_DIRECTORY talloc.h REQUIRED)

# rpc_worker_main is the common entry point used by Samba's rpcd helper
# processes. Homebrew supplies an ordinary development link in its private
# library directory. Debian installs only the versioned runtime DSO in its
# multiarch samba directory, so the second name is the exact packaged filename.
find_library(RPC_WORKER_LIBRARY
    NAMES
        RPC-WORKER-private-samba
        libRPC-WORKER-private-samba.so.0
    PATH_SUFFIXES private samba
    REQUIRED
)
find_library(DCERPC_SERVER_CORE_LIBRARY dcerpc-server-core REQUIRED)
find_library(DCERPC_BINDING_LIBRARY dcerpc-binding REQUIRED)
find_library(NDR_LIBRARY ndr REQUIRED)
find_library(SAMBA_UTIL_LIBRARY samba-util REQUIRED)
find_library(TALLOC_LIBRARY talloc REQUIRED)

set(PIDL_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(PIDL_OUTPUTS
    "${PIDL_OUTPUT_DIRECTORY}/devicefs_block_device.h"
    "${PIDL_OUTPUT_DIRECTORY}/ndr_devicefs_block_device.c"
    "${PIDL_OUTPUT_DIRECTORY}/ndr_devicefs_block_device_client.c"
    "${PIDL_OUTPUT_DIRECTORY}/ndr_devicefs_block_device_client.h"
    "${PIDL_OUTPUT_DIRECTORY}/ndr_devicefs_block_device.h"
    "${PIDL_OUTPUT_DIRECTORY}/ndr_devicefs_block_device_dispatch.c"
)
add_custom_command(
    OUTPUT ${PIDL_OUTPUTS}
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${PIDL_OUTPUT_DIRECTORY}"
    COMMAND "${CMAKE_COMMAND}" -E env
        "CPP=${CMAKE_C_COMPILER} -E"
        "${PIDL_EXECUTABLE}"
        --quiet
        --outputdir=.
        --header
        --ndr-parser
        --client=ndr_devicefs_block_device_client.c
        --server=ndr_devicefs_block_device_dispatch.c
        --
        "${CMAKE_CURRENT_SOURCE_DIR}/devicefs_block_device.idl"
    WORKING_DIRECTORY "${PIDL_OUTPUT_DIRECTORY}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/devicefs_block_device.idl"
        "${PIDL_EXECUTABLE}"
    VERBATIM
)
add_custom_target(devicefs-samba-rpc-pidl DEPENDS ${PIDL_OUTPUTS})

add_executable(rpcd_devicefs
    compat/pidl_server.c
    helper.cpp
    "${PIDL_OUTPUT_DIRECTORY}/ndr_devicefs_block_device.c"
)
add_dependencies(rpcd_devicefs devicefs-samba-rpc-pidl)
target_compile_features(rpcd_devicefs PRIVATE cxx_std_23)
set_property(TARGET rpcd_devicefs PROPERTY CXX_MODULE_STD ON)

target_include_directories(rpcd_devicefs PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_include_directories(rpcd_devicefs SYSTEM PRIVATE
    "${SAMBA_INCLUDE_DIRECTORY}"
    "${TALLOC_INCLUDE_DIRECTORY}"
)
target_compile_definitions(rpcd_devicefs PRIVATE
    HAVE_IMMEDIATE_STRUCTURES=1
    _GNU_SOURCE=1
)
target_compile_options(rpcd_devicefs PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror;-Wno-reserved-module-identifier;-Wno-reserved-user-defined-literal>
)
set_source_files_properties(
    "${PIDL_OUTPUT_DIRECTORY}/ndr_devicefs_block_device.c"
    "${PIDL_OUTPUT_DIRECTORY}/ndr_devicefs_block_device_client.c"
    PROPERTIES COMPILE_OPTIONS
        "-Wno-unused-parameter;-Wno-sign-compare;-include;sys/types.h;-include;inttypes.h"
)

target_link_libraries(rpcd_devicefs PRIVATE
    "${RPC_WORKER_LIBRARY}"
    "${DCERPC_SERVER_CORE_LIBRARY}"
    "${DCERPC_BINDING_LIBRARY}"
    "${NDR_LIBRARY}"
    "${SAMBA_UTIL_LIBRARY}"
    "${TALLOC_LIBRARY}"
)

include(CTest)
if(BUILD_TESTING)
    find_library(DCERPC_LIBRARY dcerpc REQUIRED)
    find_library(SAMBA_CREDENTIALS_LIBRARY samba-credentials REQUIRED)
    find_library(SAMBA_ERRORS_LIBRARY samba-errors REQUIRED)
    find_library(SAMBA_HOSTCONFIG_LIBRARY samba-hostconfig REQUIRED)
    find_library(TEVENT_LIBRARY tevent REQUIRED)
    find_library(TEVENT_UTIL_LIBRARY tevent-util REQUIRED)
    find_program(MKTEMP_EXECUTABLE NAMES gmktemp mktemp REQUIRED)
    find_program(PDBEDIT_EXECUTABLE NAMES pdbedit REQUIRED)
    find_program(POWERSHELL_EXECUTABLE NAMES pwsh-preview pwsh REQUIRED)
    find_program(SAMBA_DCERPCD_EXECUTABLE
        NAMES samba-dcerpcd
        HINTS /opt/homebrew/opt/samba/libexec/samba
        PATH_SUFFIXES libexec/samba
        REQUIRED
    )
    if(APPLE)
        find_program(HDIUTIL_EXECUTABLE NAMES hdiutil REQUIRED)
        set(BLOCK_DEVICE_ARGUMENTS
            -HdiutilPath "${HDIUTIL_EXECUTABLE}"
        )
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_program(LOSETUP_EXECUTABLE NAMES losetup
            PATH_SUFFIXES sbin
            REQUIRED
        )
        set(BLOCK_DEVICE_ARGUMENTS
            -LosetupPath "${LOSETUP_EXECUTABLE}"
        )
    else()
        message(FATAL_ERROR
            "The authenticated Samba RPC test requires Linux or macOS"
        )
    endif()

    add_executable(rpcd_devicefs_test_client
        tests/client.cpp
        tests/compat/credentials.c
        "${PIDL_OUTPUT_DIRECTORY}/ndr_devicefs_block_device.c"
        "${PIDL_OUTPUT_DIRECTORY}/ndr_devicefs_block_device_client.c"
    )
    add_dependencies(rpcd_devicefs_test_client devicefs-samba-rpc-pidl)
    target_compile_features(rpcd_devicefs_test_client PRIVATE cxx_std_23)
    set_property(TARGET rpcd_devicefs_test_client PROPERTY CXX_MODULE_STD ON)
    target_include_directories(rpcd_devicefs_test_client PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_include_directories(rpcd_devicefs_test_client SYSTEM PRIVATE
        "${SAMBA_INCLUDE_DIRECTORY}"
        "${TALLOC_INCLUDE_DIRECTORY}"
    )
    target_compile_definitions(rpcd_devicefs_test_client PRIVATE
        HAVE_IMMEDIATE_STRUCTURES=1
        _GNU_SOURCE=1
    )
    target_compile_options(rpcd_devicefs_test_client PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror;-Wno-reserved-module-identifier;-Wno-reserved-user-defined-literal>
    )
    target_link_libraries(rpcd_devicefs_test_client PRIVATE
        "${DCERPC_LIBRARY}"
        "${DCERPC_BINDING_LIBRARY}"
        "${NDR_LIBRARY}"
        "${SAMBA_CREDENTIALS_LIBRARY}"
        "${SAMBA_ERRORS_LIBRARY}"
        "${SAMBA_HOSTCONFIG_LIBRARY}"
        "${TALLOC_LIBRARY}"
        "${TEVENT_LIBRARY}"
        "${TEVENT_UTIL_LIBRARY}"
    )

    add_test(NAME rpcd_devicefs_authenticated
        COMMAND "${POWERSHELL_EXECUTABLE}"
            -NoLogo -NoProfile -NonInteractive -File
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixture.ps1"
            -MktempPath "${MKTEMP_EXECUTABLE}"
            -PdbEditPath "${PDBEDIT_EXECUTABLE}"
            -SambaDcerpcdPath "${SAMBA_DCERPCD_EXECUTABLE}"
            -HelperPath "$<TARGET_FILE:rpcd_devicefs>"
            -ClientPath "$<TARGET_FILE:rpcd_devicefs_test_client>"
            ${BLOCK_DEVICE_ARGUMENTS}
    )
    set_tests_properties(rpcd_devicefs_authenticated PROPERTIES TIMEOUT 30)
endif()
