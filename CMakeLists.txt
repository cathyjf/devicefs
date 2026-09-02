cmake_minimum_required(VERSION 4.2)

project(devicefs LANGUAGES C CXX RC)

if((NOT DEFINED CMAKE_VS_VERSION_BUILD_NUMBER)
        OR (CMAKE_VS_VERSION_BUILD_NUMBER VERSION_LESS 18))
    message(FATAL_ERROR "devicefs requires Visual Studio 2026 or newer")
endif()

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR "devicefs requires the Microsoft Visual C++ compiler")
endif()

if(MSVC_VERSION LESS 1951)
    # MSVC Build Tools 14.51 is required for explicit lifecycle management.
    # See <https://devblogs.microsoft.com/cppblog/c23-support-in-msvc-build-tools-14-51>.
    # See also <https://learn.microsoft.com/en-us/cpp/overview/compiler-versions>.
    message(FATAL_ERROR "devicefs requires MSVC Build Tools 14.51 or newer")
endif()

if(NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "devicefs requires an x64 target")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

# Increase build parallelism for native C++ projects.
# See <https://devblogs.microsoft.com/cppblog/cpp-build-throughput-investigation-and-tune-up>.
set(CMAKE_VS_GLOBALS
    "UseMultiToolTask=true"
    "EnableClServerMode=true"
    "BuildPassReferences=true"
)

# Build multiple source files concurrently.
# See <https://learn.microsoft.com/en-us/cpp/build/reference/mp-build-with-multiple-processes>.
add_compile_options(/MP)

option(DEVICEFS_MEASURE_FREE_CLUSTER_DATA
    "Measure nonzero data in bitmap-free clusters (diagnostic builds only)"
    OFF
)
option(DEVICEFS_MEASURE_READ_PATH
    "Measure the filesystem read path without changing its behavior"
    OFF
)

cmake_host_system_information(
    RESULT WINFSP_ROOT
    QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/WinFsp"
    VALUE InstallDir
    VIEW 32
)

if(NOT WINFSP_ROOT)
    message(FATAL_ERROR "WinFsp is not installed")
endif()

set(WINFSP_INCLUDE_DIR "${WINFSP_ROOT}/inc")
set(WINFSP_LIBRARY "${WINFSP_ROOT}/lib/winfsp-x64.lib")
set(WIL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dependencies/wil/include")

function(target_module_interfaces target visibility)
    target_sources(${target} ${visibility}
        FILE_SET CXX_MODULES
        FILES ${ARGN}
    )
endfunction()

if(NOT EXISTS "${WINFSP_INCLUDE_DIR}/winfsp/winfsp.h" OR NOT EXISTS "${WINFSP_LIBRARY}")
    message(FATAL_ERROR
        "WinFsp developer files were not found; reinstall WinFsp with the Developer feature"
    )
endif()

add_library(devicefs-common STATIC)
target_module_interfaces(devicefs-common PUBLIC
    src/modules/common.ixx
    src/modules/stream_writer.ixx
)
target_sources(devicefs-common PRIVATE
    src/modules/stream_writer.cpp
)

set(DEVICEFS_RPC_DEFINITION
    "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/filesystem/rpc_block_device.idl"
)
set(DEVICEFS_RPC_OUTPUT_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/devicefs"
)
set(DEVICEFS_RPC_HEADER
    "${DEVICEFS_RPC_OUTPUT_DIRECTORY}/rpc_block_device.h"
)
set(DEVICEFS_RPC_CLIENT_STUB
    "${DEVICEFS_RPC_OUTPUT_DIRECTORY}/rpc_block_device_c.c"
)
set(DEVICEFS_RPC_SERVER_STUB
    "${DEVICEFS_RPC_OUTPUT_DIRECTORY}/rpc_block_device_s.c"
)
add_custom_command(
    OUTPUT
        "${DEVICEFS_RPC_HEADER}"
        "${DEVICEFS_RPC_CLIENT_STUB}"
        "${DEVICEFS_RPC_SERVER_STUB}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${DEVICEFS_RPC_OUTPUT_DIRECTORY}"
    COMMAND midl.exe
        /env "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}"
        /error none
        /Os
        /sal
        /prefix client DeviceFsRpcClient_
        /no_default_epv
        /client stub
        /server stub
        /out "${DEVICEFS_RPC_OUTPUT_DIRECTORY}"
        /h rpc_block_device.h
        /cstub rpc_block_device_c.c
        /sstub rpc_block_device_s.c
        "${DEVICEFS_RPC_DEFINITION}"
    MAIN_DEPENDENCY "${DEVICEFS_RPC_DEFINITION}"
    VERBATIM
)
add_library(devicefs-rpc-block-device STATIC
    "${DEVICEFS_RPC_HEADER}"
    "${DEVICEFS_RPC_CLIENT_STUB}"
    "${DEVICEFS_RPC_SERVER_STUB}"
)
target_module_interfaces(devicefs-rpc-block-device PUBLIC
    src/modules/rpc_constants.ixx
)
set_source_files_properties(
    "${DEVICEFS_RPC_CLIENT_STUB}"
    "${DEVICEFS_RPC_SERVER_STUB}"
    PROPERTIES COMPILE_OPTIONS "/wd4213;/wd6011"
)
target_include_directories(devicefs-rpc-block-device PUBLIC
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(devicefs-rpc-block-device PUBLIC rpcrt4)

add_library(devicefs-filesystem STATIC)
target_module_interfaces(devicefs-filesystem PUBLIC
    src/modules/filesystem.ixx
    src/modules/filesystem/internal.cpp
    src/modules/filesystem/rpc_client.cpp
    src/modules/rpc_block_device_server.ixx
    src/modules/synthetic_backup_block_device.ixx
    src/modules/vhdx_viewer.ixx
    src/modules/windows_block_device.ixx
)
target_sources(devicefs-filesystem PRIVATE
    src/modules/filesystem_allocation_changes.cpp
)
target_link_libraries(devicefs-filesystem PRIVATE
    devicefs-common devicefs-rpc-block-device ole32
)

add_library(devicefs-vss-block-descriptors STATIC)
target_module_interfaces(devicefs-vss-block-descriptors PUBLIC
    src/modules/svi_extents.ixx
    src/modules/vss_block_descriptors.ixx
)
target_link_libraries(devicefs-vss-block-descriptors PRIVATE devicefs-common)

add_library(vshadow STATIC
    dependencies/vshadow/create.cpp
    dependencies/vshadow/query.cpp
    dependencies/vshadow/select.cpp
    dependencies/vshadow/tracing.cpp
    dependencies/vshadow/vssclient.cpp
    dependencies/vshadow/writer.cpp
)
target_include_directories(vshadow SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/dependencies"
)
target_compile_definitions(vshadow PRIVATE
    UNICODE _UNICODE _WIN32_WINNT=0x0A00 _MSVC_STL_HARDENING=1
)
target_compile_options(vshadow PRIVATE
    /W3 /WX- /permissive /EHsc /utf-8 /guard:cf /guard:ehcont /sdl
    /analyze-
)
set_target_properties(vshadow PROPERTIES
    VS_GLOBAL_EnableCppCoreCheck false
    VS_GLOBAL_RunCodeAnalysis false
)
target_link_libraries(vshadow PRIVATE
    ole32 oleaut32 uuid vssapi
)

add_executable(devicefs src/main.cpp)
target_compile_definitions(devicefs PRIVATE DEVICEFS_PROGRAM_DEVICEFS)
target_link_libraries(devicefs PRIVATE devicefs-common devicefs-filesystem)

add_executable(vss-descriptor-dump
    src/main.cpp
    src/vss_descriptor_dump.cpp
)
target_compile_definitions(vss-descriptor-dump PRIVATE
    DEVICEFS_PROGRAM_VSS_DESCRIPTOR_DUMP
)
target_link_libraries(vss-descriptor-dump PRIVATE
    devicefs-common devicefs-vss-block-descriptors ole32
)

add_executable(backup-supervisor
    src/main.cpp
    src/backup_supervisor.cpp
)
target_link_libraries(backup-supervisor PRIVATE
    devicefs-common devicefs-filesystem devicefs-rpc-block-device
    devicefs-vss-block-descriptors vshadow
)

set(START_PBS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/supervisor/artifacts/start-pbs.fish"
)
set(ARTIFACT_RESOURCE_SCRIPT
    "${CMAKE_CURRENT_BINARY_DIR}/backup-supervisor-artifacts.rc"
)
file(GENERATE OUTPUT "${ARTIFACT_RESOURCE_SCRIPT}" CONTENT
    "START_PBS DEVICEFS_ARTIFACT \"${START_PBS}\"\n"
)
target_sources(backup-supervisor PRIVATE "${ARTIFACT_RESOURCE_SCRIPT}")
target_module_interfaces(backup-supervisor PRIVATE
    src/modules/supervisor/configuration.ixx
    src/modules/supervisor/embedded_artifacts.ixx
    src/modules/supervisor/find_powershell.ixx
    src/modules/supervisor/installation.ixx
    src/modules/supervisor/logging_console.ixx
    src/modules/supervisor/native_backup.ixx
    src/modules/supervisor/native_backup/backup_viewer.cpp
    src/modules/supervisor/native_backup/devicefs_process.cpp
    src/modules/supervisor/native_backup/filesystem_verifier.cpp
    src/modules/supervisor/native_backup/incremental.cpp
    src/modules/supervisor/native_backup/incremental_diagnostics.cpp
    src/modules/supervisor/native_backup/internal.cpp
    src/modules/supervisor/native_backup/manifest.cpp
    src/modules/supervisor/native_backup/pbs.cpp
    src/modules/supervisor/native_backup/password_reset.cpp
    src/modules/supervisor/native_backup/port_selection.cpp
    src/modules/supervisor/native_backup/privileges.cpp
    src/modules/supervisor/native_backup/s4u_logon.cpp
    src/modules/supervisor/native_backup/vhdx_attachment.cpp
    src/modules/supervisor/process_diagnostics.ixx
    src/modules/supervisor/vshadow.ixx
    src/modules/supervisor/winrt_apartment.ixx
)

if(DEVICEFS_MEASURE_FREE_CLUSTER_DATA OR DEVICEFS_MEASURE_READ_PATH)
    target_module_interfaces(devicefs-filesystem PUBLIC
        src/modules/filesystem_measurement.ixx
    )
endif()

set(DEVICEFS_CODE_ANALYSIS_TARGETS
    devicefs-common
    devicefs-rpc-block-device
    devicefs-filesystem
    devicefs-vss-block-descriptors
    devicefs
    vss-descriptor-dump
    backup-supervisor
)
foreach(target IN LISTS DEVICEFS_CODE_ANALYSIS_TARGETS)
    set_target_properties(${target} PROPERTIES
        VS_GLOBAL_EnableCppCoreCheck true
        VS_GLOBAL_RunCodeAnalysis true
        VS_GLOBAL_CodeAnalysisRuleSet "${CMAKE_CURRENT_SOURCE_DIR}/.ruleset"
        VS_GLOBAL_CAExcludePath "${WIL_INCLUDE_DIR};$(CAExcludePath)"
    )
    target_include_directories(${target} SYSTEM PRIVATE "${WIL_INCLUDE_DIR}")
    target_include_directories(${target} PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/include"
    )
    target_compile_definitions(${target} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _MSVC_STL_HARDENING=1
    )
    target_compile_options(${target} PRIVATE
        /W4 /WX /permissive- /EHsc /utf-8 /guard:cf /guard:ehcont /sdl
        /analyze:external- "SHELL:/analyze:log $(IntDir)"
        /analyze:log:format:sarif /analyze:sarif:configuration
        /analyze:log:compilerwarnings
    )
endforeach()

foreach(target devicefs vss-descriptor-dump backup-supervisor)
    target_sources(${target} PRIVATE src/resources/devicefs.manifest)
    target_link_options(${target} PRIVATE
        /GUARD:CF /guard:ehcont /CETCOMPAT
    )
endforeach()

set_target_properties(devicefs-filesystem PROPERTIES
    VS_GLOBAL_CAExcludePath "${WINFSP_INCLUDE_DIR};${WIL_INCLUDE_DIR};$(CAExcludePath)"
)

target_include_directories(devicefs-filesystem SYSTEM PRIVATE
    "${WINFSP_INCLUDE_DIR}"
)
target_link_libraries(devicefs-filesystem INTERFACE
    "${WINFSP_LIBRARY}" advapi32 delayimp
)
target_link_options(devicefs-filesystem INTERFACE
    /DELAYLOAD:winfsp-x64.dll
)
target_compile_definitions(devicefs-filesystem PRIVATE
    DEVICEFS_MEASURE_FREE_CLUSTER_DATA=$<BOOL:${DEVICEFS_MEASURE_FREE_CLUSTER_DATA}>
    DEVICEFS_MEASURE_READ_PATH=$<BOOL:${DEVICEFS_MEASURE_READ_PATH}>
)

target_compile_definitions(backup-supervisor PRIVATE _WIN32_WINNT=0x0A00)
target_link_libraries(backup-supervisor PRIVATE
    advapi32 bcrypt netapi32 ntdll ole32 secur32 shell32 userenv virtdisk windowsapp ws2_32
)

find_program(POWERSHELL_EXECUTABLE NAMES pwsh REQUIRED)
add_custom_target(check-native-code-analysis ALL
    COMMAND "${POWERSHELL_EXECUTABLE}"
        -NoLogo -NoProfile -NonInteractive -File
        "${CMAKE_CURRENT_SOURCE_DIR}/misc/Check-NativeCodeAnalysis.ps1"
        -BuildDirectory "${CMAKE_CURRENT_BINARY_DIR}"
        -Configuration "$<CONFIG>"
    USES_TERMINAL
    VERBATIM
)
add_dependencies(check-native-code-analysis
    ${DEVICEFS_CODE_ANALYSIS_TARGETS}
)
