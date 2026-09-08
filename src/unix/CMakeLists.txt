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

include(CTest)

add_subdirectory(samba_rpc)
