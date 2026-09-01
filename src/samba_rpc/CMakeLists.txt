# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 4.4)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_LIST_DIR}/cmake/llvm.cmake"
        CACHE FILEPATH "DeviceFs Samba RPC LLVM toolchain"
    )
endif()

project(devicefs-samba-rpc LANGUAGES C CXX)

option(DEVICEFS_ENABLE_GRPC_TRANSPORT
    "Enable the gRPC backing-device transport"
    OFF
)

if(DEVICEFS_ENABLE_GRPC_TRANSPORT)
    find_package(Protobuf CONFIG QUIET)
    if(NOT Protobuf_FOUND)
        find_package(Protobuf MODULE REQUIRED)
    endif()
    find_package(gRPC CONFIG REQUIRED)
    if(APPLE)
        # This component uses Homebrew LLVM because Apple Clang is dated.
        # The zlib dependency brought in by gRPC assumes the usual Apple-Clang SDK
        # layout: ZLIB::ZLIB exports the Xcode SDK's usr/include as its only
        # interface include. With Homebrew Clang that path becomes an explicit
        # -isystem entry ahead of Homebrew libc++; <cmath> consequently finds the
        # SDK's math.h instead of libc++'s math.h wrapper and fails with "<cmath>
        # tried including <math.h> but didn't find libc++'s <math.h> header."
        #
        # Homebrew Clang already obtains C headers from its selected SDK, so remove
        # only this incorrect usage requirement; ZLIB::ZLIB still supplies libz.
        # Switching to Apple Clang would lose the required standard-library module
        # support, while setting CMAKE_OSX_SYSROOT would propagate still more Xcode
        # SDK paths through every target to compensate for one defective imported
        # target.
        set_property(TARGET ZLIB::ZLIB PROPERTY INTERFACE_INCLUDE_DIRECTORIES "")
    endif()
endif()

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
# processes. Homebrew supplies an ordinary library name in its private
# directory. Debian installs only the versioned runtime DSO in its multiarch
# samba directory, so the second name is the exact packaged filename.
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
find_library(TEVENT_LIBRARY tevent REQUIRED)
find_package(Threads REQUIRED)

if(DEVICEFS_ENABLE_GRPC_TRANSPORT)
    set(GRPC_PROTO "${CMAKE_CURRENT_SOURCE_DIR}/idl/block-device.proto")
    set(GRPC_PROTO_OUTPUTS
        "${CMAKE_CURRENT_BINARY_DIR}/generated/block-device.pb.cc"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/block-device.pb.h"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/block-device.grpc.pb.cc"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/block-device.grpc.pb.h"
    )
    add_custom_command(
        OUTPUT ${GRPC_PROTO_OUTPUTS}
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_CURRENT_BINARY_DIR}/generated"
        COMMAND protobuf::protoc
            "--proto_path=${CMAKE_CURRENT_SOURCE_DIR}/idl"
            "--cpp_out=${CMAKE_CURRENT_BINARY_DIR}/generated"
            "--grpc_out=${CMAKE_CURRENT_BINARY_DIR}/generated"
            "--plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>"
            "${GRPC_PROTO}"
        DEPENDS
            "${GRPC_PROTO}"
            protobuf::protoc
            gRPC::grpc_cpp_plugin
        VERBATIM
    )
    add_library(devicefs-grpc-protocol STATIC ${GRPC_PROTO_OUTPUTS})
    target_compile_features(devicefs-grpc-protocol PRIVATE cxx_std_23)
    set_source_files_properties(${GRPC_PROTO_OUTPUTS}
        PROPERTIES COMPILE_OPTIONS "-w"
    )
    target_include_directories(devicefs-grpc-protocol SYSTEM PUBLIC
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
    )
    target_link_libraries(devicefs-grpc-protocol PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
    )
endif()

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
        "${CMAKE_CURRENT_SOURCE_DIR}/idl/devicefs_block_device.idl"
    WORKING_DIRECTORY "${PIDL_OUTPUT_DIRECTORY}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/idl/devicefs_block_device.idl"
        "${PIDL_EXECUTABLE}"
    VERBATIM
)
add_custom_target(devicefs-samba-rpc-pidl DEPENDS ${PIDL_OUTPUTS})

add_executable(rpcd_devicefs
    compat/pidl_server.c
    helper.cpp
    "${PIDL_OUTPUT_DIRECTORY}/ndr_devicefs_block_device.c"
)
add_dependencies(rpcd_devicefs
    devicefs-samba-rpc-pidl
)
if(DEVICEFS_ENABLE_GRPC_TRANSPORT)
    add_dependencies(rpcd_devicefs devicefs-grpc-protocol)
endif()
target_compile_features(rpcd_devicefs PRIVATE cxx_std_23)

target_include_directories(rpcd_devicefs PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_include_directories(rpcd_devicefs SYSTEM PRIVATE
    "${SAMBA_INCLUDE_DIRECTORY}"
    "${TALLOC_INCLUDE_DIRECTORY}"
)
target_compile_definitions(rpcd_devicefs PRIVATE
    DEVICEFS_ENABLE_GRPC_TRANSPORT=$<BOOL:${DEVICEFS_ENABLE_GRPC_TRANSPORT}>
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
    "${DCERPC_SERVER_CORE_LIBRARY}"
    "${DCERPC_BINDING_LIBRARY}"
    "${NDR_LIBRARY}"
    "${SAMBA_UTIL_LIBRARY}"
    "${TALLOC_LIBRARY}"
    "${TEVENT_LIBRARY}"
    Threads::Threads
)
if(DEVICEFS_ENABLE_GRPC_TRANSPORT)
    target_link_libraries(rpcd_devicefs PRIVATE devicefs-grpc-protocol)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_path(TDB_INCLUDE_DIRECTORY tdb.h REQUIRED)
    target_sources(rpcd_devicefs PRIVATE compat/tdb_open.cpp)
    target_include_directories(rpcd_devicefs SYSTEM PRIVATE
        "${TDB_INCLUDE_DIRECTORY}"
    )
    target_compile_definitions(rpcd_devicefs PRIVATE
        DEVICEFS_RPC_WORKER_LIBRARY="${RPC_WORKER_LIBRARY}"
    )
    # The Samba worker calls tdb_open_ex through ELF symbol lookup. Export
    # rpcd_devicefs's replacement so lookup selects it before libtdb; the
    # replacement uses RTLD_NEXT to call libtdb after changing the flags.
    target_link_options(rpcd_devicefs PRIVATE
        "LINKER:--export-dynamic-symbol=tdb_open_ex"
    )
    target_link_libraries(rpcd_devicefs PRIVATE ${CMAKE_DL_LIBS})
else()
    target_link_libraries(rpcd_devicefs PRIVATE "${RPC_WORKER_LIBRARY}")
endif()

include(CTest)
if(BUILD_TESTING)
    find_library(DCERPC_LIBRARY dcerpc REQUIRED)
    find_library(SAMBA_CREDENTIALS_LIBRARY samba-credentials REQUIRED)
    find_library(SAMBA_ERRORS_LIBRARY samba-errors REQUIRED)
    find_library(SAMBA_HOSTCONFIG_LIBRARY samba-hostconfig REQUIRED)
    find_library(TEVENT_UTIL_LIBRARY tevent-util REQUIRED)
    find_program(MKTEMP_EXECUTABLE NAMES gmktemp mktemp REQUIRED)
    find_program(PDBEDIT_EXECUTABLE NAMES pdbedit REQUIRED)
    find_program(POWERSHELL_EXECUTABLE NAMES pwsh-preview pwsh REQUIRED)
    set(DEVICEFS_EXECUTABLE
        "${CMAKE_CURRENT_SOURCE_DIR}/../../build/windows-x64/Release/devicefs.exe"
        CACHE FILEPATH "DeviceFs executable used by the interactive fixture"
    )
    find_program(SAMBA_DCERPCD_EXECUTABLE
        NAMES samba-dcerpcd
        HINTS /opt/homebrew/opt/samba/libexec/samba
        PATH_SUFFIXES libexec/samba
        REQUIRED
    )
    if(APPLE)
        find_program(HDIUTIL_EXECUTABLE NAMES hdiutil REQUIRED)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        find_program(LOSETUP_EXECUTABLE NAMES losetup
            PATH_SUFFIXES sbin
            REQUIRED
        )
        find_program(MKFS_FAT_EXECUTABLE NAMES mkfs.fat
            PATH_SUFFIXES sbin
            REQUIRED
        )
        find_program(MOUNT_EXECUTABLE NAMES mount
            PATH_SUFFIXES bin sbin
            REQUIRED
        )
        find_program(UMOUNT_EXECUTABLE NAMES umount
            PATH_SUFFIXES bin sbin
            REQUIRED
        )
        find_program(WINDOWS_POWERSHELL_EXECUTABLE NAMES pwsh.exe)
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
            "${CMAKE_BINARY_DIR}/CMakeCache.txt"
            -TestClient
    )
    set_tests_properties(rpcd_devicefs_authenticated PROPERTIES TIMEOUT 30)

    if(DEVICEFS_ENABLE_GRPC_TRANSPORT)
        add_executable(rpcd_devicefs_grpc_test_server
            tests/grpc_server.cpp
        )
        add_dependencies(rpcd_devicefs_grpc_test_server
            devicefs-grpc-protocol
        )
        target_compile_features(
            rpcd_devicefs_grpc_test_server PRIVATE cxx_std_23)
        target_compile_options(rpcd_devicefs_grpc_test_server PRIVATE
            $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror;-Wno-reserved-module-identifier;-Wno-reserved-user-defined-literal>
        )
        target_link_libraries(rpcd_devicefs_grpc_test_server PRIVATE
            devicefs-grpc-protocol
        )

        add_test(NAME rpcd_devicefs_grpc_authenticated
            COMMAND "${POWERSHELL_EXECUTABLE}"
                -NoLogo -NoProfile -NonInteractive -File
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixture.ps1"
                "${CMAKE_BINARY_DIR}/CMakeCache.txt"
                -TestClient
                -GrpcBacking
        )
        set_tests_properties(
            rpcd_devicefs_grpc_authenticated PROPERTIES TIMEOUT 30)
    endif()
endif()
