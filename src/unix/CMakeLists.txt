# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 4.4)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_LIST_DIR}/cmake/llvm.cmake"
        CACHE FILEPATH "DeviceFs Unix LLVM toolchain"
    )
endif()

project(devicefs-unix LANGUAGES C CXX)

option(DEVICEFS_BUILD_SAMBA_RPC "Build the Samba RPC component" ON)

include(CTest)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(devicefs-wsl-startup-broker wsl_startup_broker.cpp)
    target_compile_features(devicefs-wsl-startup-broker PRIVATE cxx_std_23)
    target_compile_options(devicefs-wsl-startup-broker PRIVATE
        -Wall -Wextra -Werror
    )
    if(BUILD_TESTING)
        find_program(POWERSHELL_EXECUTABLE NAMES pwsh-preview pwsh REQUIRED)
        find_program(UNSHARE_EXECUTABLE NAMES unshare REQUIRED)
        find_program(MOUNT_EXECUTABLE NAMES mount REQUIRED)
        find_program(FINDMNT_EXECUTABLE NAMES findmnt REQUIRED)
        add_test(NAME devicefs_wsl_startup_broker
            COMMAND "${POWERSHELL_EXECUTABLE}"
                -NoLogo -NoProfile -File
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/wsl_startup_broker.ps1"
                "${CMAKE_BINARY_DIR}/CMakeCache.txt"
        )
        set_tests_properties(devicefs_wsl_startup_broker PROPERTIES TIMEOUT 30)
    endif()
endif()

if(DEVICEFS_BUILD_SAMBA_RPC)
    add_subdirectory(samba_rpc)
endif()
