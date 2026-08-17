cmake_minimum_required(VERSION 4.2)

project(devicefs LANGUAGES CXX RC)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR "devicefs requires the Microsoft Visual C++ compiler")
endif()

if(NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "devicefs requires an x64 target")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

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

if(NOT EXISTS "${WINFSP_INCLUDE_DIR}/winfsp/winfsp.h" OR NOT EXISTS "${WINFSP_LIBRARY}")
    message(FATAL_ERROR
        "WinFsp developer files were not found; reinstall WinFsp with the Developer feature"
    )
endif()

add_library(devicefs-filesystem STATIC)
target_sources(devicefs-filesystem PUBLIC
    FILE_SET CXX_MODULES
    FILES
        src/modules/common.ixx
        src/modules/filesystem.ixx
)

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
target_link_libraries(devicefs PRIVATE devicefs-filesystem)

add_executable(backup-supervisor
    src/backup_supervisor.cpp
    src/modules/supervisor/native_backup_fifo.cpp
)
target_link_libraries(backup-supervisor PRIVATE devicefs-filesystem vshadow)

set(START_PBS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/supervisor/artifacts/start-pbs.fish"
)
set(DEVICE_TO_FIFO
    "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/supervisor/artifacts/device-to-fifo.fish"
)
set(ARTIFACT_RESOURCE_SCRIPT
    "${CMAKE_CURRENT_BINARY_DIR}/backup-supervisor-artifacts-$<CONFIG>.rc"
)
set(ARTIFACT_RESOURCE
    "${CMAKE_CURRENT_BINARY_DIR}/backup-supervisor-artifacts-$<CONFIG>.res"
)
file(GENERATE OUTPUT "${ARTIFACT_RESOURCE_SCRIPT}" CONTENT
    "START_PBS DEVICEFS_ARTIFACT \"${START_PBS}\"
    DEVICE_TO_FIFO DEVICEFS_ARTIFACT \"${DEVICE_TO_FIFO}\"\n"
)
add_custom_command(
    OUTPUT "${ARTIFACT_RESOURCE}"
    COMMAND "${CMAKE_RC_COMPILER}" /nologo /fo "${ARTIFACT_RESOURCE}"
        "${ARTIFACT_RESOURCE_SCRIPT}"
    DEPENDS "${ARTIFACT_RESOURCE_SCRIPT}" "${START_PBS}" "${DEVICE_TO_FIFO}"
    VERBATIM
)
add_custom_target(backup-supervisor-artifacts
    DEPENDS "${ARTIFACT_RESOURCE}"
)
add_dependencies(backup-supervisor backup-supervisor-artifacts)
target_link_libraries(backup-supervisor PRIVATE "${ARTIFACT_RESOURCE}")
target_sources(backup-supervisor PRIVATE
    FILE_SET CXX_MODULES
    FILES
        src/modules/supervisor/configuration.ixx
        src/modules/supervisor/embedded_artifacts.ixx
        src/modules/supervisor/installation.ixx
        src/modules/supervisor/logging_console.ixx
        src/modules/supervisor/native_backup.ixx
        src/modules/supervisor/process_diagnostics.ixx
        src/modules/supervisor/vshadow.ixx
)

if(DEVICEFS_MEASURE_FREE_CLUSTER_DATA OR DEVICEFS_MEASURE_READ_PATH)
    target_sources(devicefs-filesystem PUBLIC
        FILE_SET measurement_modules
        TYPE CXX_MODULES
        FILES src/modules/filesystem_measurement.ixx
    )
endif()

foreach(target devicefs-filesystem devicefs backup-supervisor)
    set_target_properties(${target} PROPERTIES
        VS_GLOBAL_EnableCppCoreCheck true
        VS_GLOBAL_RunCodeAnalysis true
        VS_GLOBAL_CodeAnalysisRuleSet "${CMAKE_CURRENT_SOURCE_DIR}/.ruleset"
        VS_GLOBAL_CAExcludePath "${WIL_INCLUDE_DIR};$(CAExcludePath)"
    )
    target_include_directories(${target} SYSTEM PRIVATE "${WIL_INCLUDE_DIR}")
    target_compile_definitions(${target} PRIVATE
        UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _MSVC_STL_HARDENING=1
    )
    target_compile_options(${target} PRIVATE
        /W4 /WX /permissive- /EHsc /utf-8 /guard:cf /guard:ehcont /sdl
        /analyze:external- /analyze:autolog-
    )
endforeach()

foreach(target devicefs backup-supervisor)
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
