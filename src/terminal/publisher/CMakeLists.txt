# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(github-release-publisher-support OBJECT)
target_sources(github-release-publisher-support PUBLIC
    FILE_SET CXX_MODULES FILES base_process.ixx release.ixx
)
target_link_libraries(github-release-publisher-support PUBLIC devicefs-terminal)
if(WIN32)
    target_sources(github-release-publisher-support PUBLIC
        FILE_SET CXX_MODULES FILES windows_process.ixx)
else()
    target_sources(github-release-publisher-support PUBLIC
        FILE_SET CXX_MODULES FILES unix_process.ixx)
endif()

add_executable(github-release-publisher main.cpp)
target_sources(github-release-publisher PRIVATE
    FILE_SET CXX_MODULES FILES preview.ixx)
target_link_libraries(github-release-publisher PRIVATE
    github-release-publisher-support devicefs-terminal)
if(WIN32)
    devicefs_version_info(github-release-publisher
        "GitHub Release Publisher" 0.1)
    target_include_directories(github-release-publisher PRIVATE
        "${DEVICEFS_ROOT}/src/modules/include")
    target_sources(github-release-publisher PRIVATE
        "${DEVICEFS_ROOT}/src/resources/devicefs.manifest")
endif()
list(APPEND DEVICEFS_TERMINAL_ANALYSIS_TARGETS
    github-release-publisher-support github-release-publisher)

if(DEVICEFS_TERMINAL_BUILD_TESTS)
    add_executable(github-release-publisher-test tests.cpp)
    target_sources(github-release-publisher-test PRIVATE
        FILE_SET CXX_MODULES BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/.."
        FILES "${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_support.ixx")
    target_link_libraries(github-release-publisher-test PRIVATE
        github-release-publisher-support devicefs-terminal)
    if(WIN32)
        target_sources(github-release-publisher-test PRIVATE
            "${DEVICEFS_ROOT}/src/resources/devicefs.manifest")
    endif()
    list(APPEND DEVICEFS_TERMINAL_ANALYSIS_TARGETS
        github-release-publisher-test)
    add_test(NAME github_release_publisher
        COMMAND github-release-publisher-test)
    set_tests_properties(github_release_publisher PROPERTIES TIMEOUT 180)
endif()

set(DEVICEFS_TERMINAL_ANALYSIS_TARGETS
    "${DEVICEFS_TERMINAL_ANALYSIS_TARGETS}" PARENT_SCOPE)
