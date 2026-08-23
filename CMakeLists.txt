cmake_minimum_required(VERSION 4.2)

set(DEVICEFS_TARGET_ARCHITECTURE x64)

# These MSVC hardening options are applied to all targets.
set(DEVICEFS_HARDENING_OPTIONS /guard:cf /guard:ehcont /sdl)

# Standard library hardening is applied to all targets.
# See <https://devblogs.microsoft.com/cppblog/whats-new-for-cpp-developers-in-visual-studio-2022-17-14/>.
set(DEVICEFS_HARDENING_DEFINITIONS _MSVC_STL_HARDENING=1)

# External dependencies specified in the vcpkg manifest are built with the same
# hardening configuration as all other project code. Here, we construct the set
# of flags that will be inserted into the generated vcpkg triplet based on the
# configuration specified above.
set(DEVICEFS_VCPKG_DEFINITION_FLAGS ${DEVICEFS_HARDENING_DEFINITIONS})
list(TRANSFORM DEVICEFS_VCPKG_DEFINITION_FLAGS PREPEND "/D")
string(JOIN " " DEVICEFS_VCPKG_FLAGS
    ${DEVICEFS_HARDENING_OPTIONS}
    ${DEVICEFS_VCPKG_DEFINITION_FLAGS}
)

set(VCPKG_TARGET_TRIPLET "${DEVICEFS_TARGET_ARCHITECTURE}-windows-devicefs")
set(VCPKG_OVERLAY_TRIPLETS "${CMAKE_BINARY_DIR}/vcpkg-triplets")
file(MAKE_DIRECTORY "${VCPKG_OVERLAY_TRIPLETS}")
file(CONFIGURE
    OUTPUT "${VCPKG_OVERLAY_TRIPLETS}/${VCPKG_TARGET_TRIPLET}.cmake"
    CONTENT [=[
set(VCPKG_TARGET_ARCHITECTURE "@DEVICEFS_TARGET_ARCHITECTURE@")
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)

set(VCPKG_C_FLAGS "@DEVICEFS_VCPKG_FLAGS@")
set(VCPKG_CXX_FLAGS "@DEVICEFS_VCPKG_FLAGS@")
]=]
    @ONLY
)

project(devicefs LANGUAGES CXX RC)

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

if(NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL
        "${DEVICEFS_TARGET_ARCHITECTURE}")
    message(FATAL_ERROR
        "devicefs requires an ${DEVICEFS_TARGET_ARCHITECTURE} target")
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

find_package(Boost 1.91.0 EXACT CONFIG REQUIRED
    COMPONENTS program_options
)

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

add_library(devicefs-filesystem STATIC)
target_module_interfaces(devicefs-filesystem PUBLIC
    src/modules/filesystem.ixx
)
target_link_libraries(devicefs-filesystem PRIVATE devicefs-common)

add_library(devicefs-vss-block-descriptors STATIC)
target_module_interfaces(devicefs-vss-block-descriptors PUBLIC
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
    UNICODE _UNICODE _WIN32_WINNT=0x0A00
    ${DEVICEFS_HARDENING_DEFINITIONS}
)
target_compile_options(vshadow PRIVATE
    /W3 /WX- /permissive /EHsc /utf-8 ${DEVICEFS_HARDENING_OPTIONS}
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
target_link_libraries(devicefs PRIVATE devicefs-common devicefs-filesystem)

add_executable(vss-descriptor-dump src/vss_descriptor_dump.cpp)
target_link_libraries(vss-descriptor-dump PRIVATE
    devicefs-common devicefs-vss-block-descriptors Boost::program_options ole32
)

add_executable(backup-supervisor
    src/backup_supervisor.cpp
)
target_link_libraries(backup-supervisor PRIVATE
    devicefs-common devicefs-filesystem vshadow
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
    src/modules/supervisor/installation.ixx
    src/modules/supervisor/logging_console.ixx
    src/modules/supervisor/native_backup.ixx
    src/modules/supervisor/native_backup_devicefs_process.cpp
    src/modules/supervisor/native_backup_internal.cpp
    src/modules/supervisor/native_backup_manifest.cpp
    src/modules/supervisor/native_backup_pbs.cpp
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
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX
        ${DEVICEFS_HARDENING_DEFINITIONS}
    )
    target_compile_options(${target} PRIVATE
        /W4 /WX /permissive- /EHsc /utf-8 ${DEVICEFS_HARDENING_OPTIONS}
        /analyze:external- "SHELL:/analyze:log $(IntDir)"
        /analyze:log:format:sarif /analyze:sarif:configuration
        /analyze:log:compilerwarnings
    )
endforeach()

foreach(target devicefs vss-descriptor-dump backup-supervisor)
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
    advapi32 ntdll ole32 shell32 userenv windowsapp
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
