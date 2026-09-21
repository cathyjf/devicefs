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

if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    set(WINFSP_ARCHITECTURE x64)
    set(DISM_ARCHITECTURE amd64)
elseif(CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "ARM64")
    set(WINFSP_ARCHITECTURE a64)
    set(DISM_ARCHITECTURE arm64)
else()
    message(FATAL_ERROR "devicefs requires an x64 or ARM64 target")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
include(misc/MsvcFlags.cmake)
include(misc/WindowsVersionInfo.cmake)

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
set(WINFSP_LIBRARY "${WINFSP_ROOT}/lib/winfsp-${WINFSP_ARCHITECTURE}.lib")
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

cmake_host_system_information(
    RESULT WINDOWS_KITS_ROOT
    QUERY WINDOWS_REGISTRY "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
    VALUE KitsRoot10
    VIEW 32
)
set(DISM_SDK_ROOT
    "${WINDOWS_KITS_ROOT}/Assessment and Deployment Kit/Deployment Tools/SDKs/DismApi"
)
set(DISM_INCLUDE_DIR "${DISM_SDK_ROOT}/Include")
set(DISM_LIBRARY "${DISM_SDK_ROOT}/Lib/${DISM_ARCHITECTURE}/DismApi.lib")
if(NOT EXISTS "${DISM_INCLUDE_DIR}/DismApi.h" OR NOT EXISTS "${DISM_LIBRARY}")
    message(FATAL_ERROR
        "DISM developer files were not found; install the Windows ADK Deployment Tools"
    )
endif()

add_library(devicefs-common STATIC src/modules/common.cpp)
target_link_libraries(devicefs-common PRIVATE oleaut32)

# The Windows headers used by `common.cpp` are expensive to compile. Making the
# rest of the program wait for that compilation unnecessarily lengthens the build:
# consumers need only the declarations in `common.h` until they reach linking.
# However, linking the CMake target creates an MSBuild project dependency that
# requires the whole library to be built before consumer compilation can begin.
# CMake's `LINK_ONLY` expression does not remove that scheduling dependency.
#
# Linking by pathname removes that unnecessary wait. The MSBuild action
# `DeviceFsBuildCommonBeforeLink`, defined in `misc/ApplyMsvcOverrides.props`,
# ensures that the common library has finished building before an executable's
# linker runs. The remaining libraries resolve the common library's external
# symbols.
set(DEVICEFS_COMMON_LINK_LIBRARIES
    "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/devicefs-common.lib"
    oleaut32 devicefs-terminal
)

find_program(POWERSHELL_EXECUTABLE NAMES pwsh REQUIRED)

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
        /env "$<LOWER_CASE:${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}>"
        /error none
        "$<IF:$<STREQUAL:${CMAKE_CXX_COMPILER_ARCHITECTURE_ID},x64>,/Os,/Oicf>"
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
    COMMAND "${POWERSHELL_EXECUTABLE}"
        -NoProfile -NonInteractive -File
        "${CMAKE_CURRENT_SOURCE_DIR}/misc/Fix-RpcServerStub.ps1"
        -ServerStub "${DEVICEFS_RPC_SERVER_STUB}"
        -TargetArchitecture "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}"
    MAIN_DEPENDENCY "${DEVICEFS_RPC_DEFINITION}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/misc/Fix-RpcServerStub.ps1"
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
    /W3 /WX- /permissive
    /analyze-
)
set_target_properties(vshadow PROPERTIES
    # VShadow uses no C++ modules. With module scanning enabled, MSBuild 18.10
    # gives all six sources the same `.d.json` dependency file and batches them
    # into one compiler invocation. The resulting dependency records are
    # incomplete, so unchanged sources are compiled again on every build.
    # Disabling the unused scan restores normal source and header tracking.
    CXX_SCAN_FOR_MODULES OFF
    VS_GLOBAL_EnableCppCoreCheck false
    VS_GLOBAL_RunCodeAnalysis false
)
target_link_libraries(vshadow PRIVATE
    ole32 oleaut32 uuid vssapi
)

add_executable(devicefs-read-path-test tests/read_path.cpp)
target_link_libraries(devicefs-read-path-test PRIVATE
    ${DEVICEFS_COMMON_LINK_LIBRARIES}
)

add_executable(devicefs-error-messages-test tests/error_messages.cpp)
target_link_libraries(devicefs-error-messages-test PRIVATE ${DEVICEFS_COMMON_LINK_LIBRARIES})

add_subdirectory(src/terminal)
target_link_libraries(devicefs-common PRIVATE devicefs-terminal)
add_executable(backup-supervisor
    src/main.cpp
    src/backup_supervisor.cpp
    src/modules/supervisor/account_management.cpp
    src/modules/filesystem_allocation_changes.cpp
    src/vss_descriptor_dump.cpp
)
devicefs_version_info(backup-supervisor "DeviceFs Backup Supervisor" 0.1)
target_link_libraries(backup-supervisor PRIVATE
    ${DEVICEFS_COMMON_LINK_LIBRARIES} devicefs-rpc-block-device
    devicefs-terminal vshadow
    Version
)

foreach(target IN ITEMS backup-supervisor devicefs-read-path-test)
    target_sources(${target}
        PRIVATE FILE_SET CXX_MODULES
            FILES
                src/modules/stream_writer.ixx
                src/modules/allocation.ixx
                src/modules/windows_block_device.ixx
        PRIVATE src/modules/stream_writer.cpp
    )
endforeach()

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
    src/modules/filesystem.ixx
    src/modules/filesystem/internal.cpp
    src/modules/filesystem/rpc_client.cpp
    src/modules/rpc_block_device_server.ixx
    src/modules/synthetic_backup_block_device.ixx
    src/modules/vhdx_viewer.ixx
    src/modules/svi_extents.ixx
    src/modules/vss_block_descriptors.ixx
    src/modules/supervisor/account_management.ixx
    src/modules/supervisor/browse.ixx
    src/modules/supervisor/configuration.ixx
    src/modules/supervisor/embedded_artifacts.ixx
    src/modules/supervisor/https_download.ixx
    src/modules/supervisor/installation.ixx
    src/modules/supervisor/launch_powershell.ixx
    src/modules/supervisor/logging_console.ixx
    src/modules/supervisor/materialize_oci.ixx
    src/modules/supervisor/native_backup.ixx
    src/modules/supervisor/native_backup/backup_viewer.cpp
    src/modules/supervisor/native_backup/devicefs_process.cpp
    src/modules/supervisor/native_backup/filesystem_verifier.cpp
    src/modules/supervisor/native_backup/incremental.cpp
    src/modules/supervisor/native_backup/incremental_diagnostics.cpp
    src/modules/supervisor/native_backup/internal.cpp
    src/modules/supervisor/native_backup/manifest.cpp
    src/modules/supervisor/native_backup/pbs.cpp
    src/modules/supervisor/native_backup/port_selection.cpp
    src/modules/supervisor/native_backup/privileges.cpp
    src/modules/supervisor/native_backup/s4u_logon.cpp
    src/modules/supervisor/native_backup/vhdx_attachment.cpp
    src/modules/supervisor/process_diagnostics.ixx
    src/modules/supervisor/process_launch.ixx
    src/modules/supervisor/temporary_paths.ixx
    src/modules/supervisor/vshadow.ixx
    src/modules/supervisor/winrt_apartment.ixx
)

foreach(target IN ITEMS backup-supervisor devicefs-read-path-test)
    if(DEVICEFS_MEASURE_FREE_CLUSTER_DATA OR DEVICEFS_MEASURE_READ_PATH)
        target_module_interfaces(${target} PRIVATE
            src/modules/filesystem_measurement.ixx
        )
    endif()
    target_compile_definitions(${target} PRIVATE
        DEVICEFS_MEASURE_FREE_CLUSTER_DATA=$<BOOL:${DEVICEFS_MEASURE_FREE_CLUSTER_DATA}>
        DEVICEFS_MEASURE_READ_PATH=$<BOOL:${DEVICEFS_MEASURE_READ_PATH}>
    )
endforeach()

set(DEVICEFS_CODE_ANALYSIS_TARGETS
    devicefs-common
    devicefs-rpc-block-device
    devicefs-read-path-test
    devicefs-error-messages-test
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
        /W4 /WX /permissive-
        /analyze:external- "SHELL:/analyze:log $(IntDir)"
        /analyze:log:format:sarif /analyze:sarif:configuration
        /analyze:log:compilerwarnings
    )
endforeach()

foreach(target devicefs-read-path-test devicefs-error-messages-test backup-supervisor)
    target_sources(${target} PRIVATE src/resources/devicefs.manifest)
    set_target_properties(${target} PROPERTIES
        VS_GLOBAL_DeviceFsCommonProject "${CMAKE_CURRENT_BINARY_DIR}/devicefs-common.vcxproj"
    )
endforeach()

set_target_properties(backup-supervisor devicefs-read-path-test PROPERTIES
    VS_GLOBAL_CAExcludePath "${WINFSP_INCLUDE_DIR};${WIL_INCLUDE_DIR};$(CAExcludePath)"
)

foreach(target IN ITEMS backup-supervisor devicefs-read-path-test)
    target_include_directories(${target} SYSTEM PRIVATE
        "${WINFSP_INCLUDE_DIR}"
    )
    target_link_libraries(${target} PRIVATE
        "${WINFSP_LIBRARY}" advapi32 delayimp
    )
    target_link_options(${target} PRIVATE
        /DELAYLOAD:winfsp-${WINFSP_ARCHITECTURE}.dll
    )
endforeach()

target_compile_definitions(backup-supervisor PRIVATE _WIN32_WINNT=0x0A00)
target_include_directories(backup-supervisor SYSTEM PRIVATE "${DISM_INCLUDE_DIR}")
target_link_libraries(backup-supervisor PRIVATE
    "${DISM_LIBRARY}"
    advapi32 bcrypt crypt32 msi netapi32 ntdll ole32 pathcch secur32 shell32 userenv virtdisk windowsapp ws2_32
)

add_custom_target(check-native-code-analysis ALL
    COMMAND "${POWERSHELL_EXECUTABLE}"
        -NoProfile -NonInteractive -File
        "${CMAKE_CURRENT_SOURCE_DIR}/misc/Check-NativeCodeAnalysis.ps1"
        -BuildDirectory "${CMAKE_CURRENT_BINARY_DIR}"
        -Configuration "$<CONFIG>"
    USES_TERMINAL
    VERBATIM
)
add_dependencies(check-native-code-analysis
    ${DEVICEFS_CODE_ANALYSIS_TARGETS}
)
