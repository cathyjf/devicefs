cmake_minimum_required(VERSION 4.2)

project(devicefs LANGUAGES CXX)

if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    message(FATAL_ERROR "devicefs requires the Microsoft Visual C++ compiler")
endif()

if(NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
    message(FATAL_ERROR "devicefs requires an x64 target")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

add_executable(devicefs
    src/main.cpp
)

set_target_properties(devicefs PROPERTIES
    VS_GLOBAL_EnableCppCoreCheck true
    VS_GLOBAL_RunCodeAnalysis true
    VS_GLOBAL_CodeAnalysisRuleSet "${CMAKE_CURRENT_SOURCE_DIR}/.ruleset"
    VS_GLOBAL_CAExcludePath "${WINFSP_INCLUDE_DIR};${WIL_INCLUDE_DIR};$(CAExcludePath)"
)

target_include_directories(devicefs SYSTEM PRIVATE
    "${WINFSP_INCLUDE_DIR}"
    "${WIL_INCLUDE_DIR}"
)
target_link_libraries(devicefs PRIVATE "${WINFSP_LIBRARY}" advapi32 delayimp)
target_link_options(devicefs PRIVATE
    /DELAYLOAD:winfsp-x64.dll /GUARD:CF /guard:ehcont /CETCOMPAT
)
target_compile_definitions(devicefs PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _MSVC_STL_HARDENING=1
)

target_compile_options(devicefs PRIVATE
    /W4 /WX /permissive- /EHsc /utf-8 /guard:cf /guard:ehcont /sdl
    /analyze:external-
)
