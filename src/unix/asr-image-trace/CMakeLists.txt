# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 4.4)

project(devicefs-asr-image-trace LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

# The `-freflection` option must be added here rather than in CMakePresets.json
# or else CMake raises an error trying to compile a sample program.
add_compile_options(-freflection)

add_library(asr-image-trace-core OBJECT)
target_sources(asr-image-trace-core PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS . ../../terminal
    FILES filesystem.ixx trace.ixx synthetic.ixx experiment.ixx
        ../../terminal/safecast.ixx
)
target_compile_options(asr-image-trace-core PUBLIC
    -Wall -Wextra -Werror
    -fcontracts -fcontract-evaluation-semantic=enforce
)
target_link_options(asr-image-trace-core PUBLIC -fcontracts)
target_link_libraries(asr-image-trace-core PUBLIC Threads::Threads)

add_executable(asr-image-trace-synthetic synthetic_main.cpp)
target_link_libraries(asr-image-trace-synthetic PRIVATE asr-image-trace-core)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME asr-image-trace-synthetic
        COMMAND asr-image-trace-synthetic
            --output-root "${CMAKE_CURRENT_BINARY_DIR}/test-results"
    )
endif()

option(DEVICEFS_BUILD_ASR_MACOS "Build the macOS ASR and macFUSE adapter" ${APPLE})
if(DEVICEFS_BUILD_ASR_MACOS)
    if(NOT APPLE)
        message(FATAL_ERROR "The real ASR adapter requires macOS")
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MACFUSE REQUIRED IMPORTED_TARGET fuse)
    find_program(FISH_EXECUTABLE fish REQUIRED)
    find_program(JQ_EXECUTABLE jq REQUIRED)
    find_program(CODESIGN_EXECUTABLE codesign REQUIRED)
    set(DEVICEFS_ASR_SIGNING_IDENTITY
        "Developer ID Application: Cathy Fitzpatrick (KVRBCYNMT7)"
        CACHE STRING "Signing identity for the native test bundle")
    option(DEVICEFS_ASR_SNAPSHOT_ENTITLEMENT
        "Claim the snapshot entitlement for fixtures in an AMFI-disabled VM" OFF)
    set(DEVICEFS_ASR_BUNDLE_IDENTIFIER "com.cathyjf.devicefs.asr-image-trace")
    set(asr_snapshot_entitlement "")
    if(DEVICEFS_ASR_SNAPSHOT_ENTITLEMENT)
        set(asr_snapshot_entitlement
            "<key>com.apple.developer.vfs.snapshot</key><true/>")
    endif()
    set(asr_entitlements "${CMAKE_CURRENT_BINARY_DIR}/entitlements.plist")
    set(asr_info "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
    set(asr_bundle "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/asr-image-trace-macos.app")
    configure_file(metadata/entitlements.plist.in "${asr_entitlements}" @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${asr_entitlements}")
    block()
        set(MACOSX_BUNDLE_EXECUTABLE_NAME asr-image-trace-macos)
        set(MACOSX_BUNDLE_GUI_IDENTIFIER "${DEVICEFS_ASR_BUNDLE_IDENTIFIER}")
        set(MACOSX_BUNDLE_BUNDLE_NAME "ASR Image Trace")
        set(MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0")
        set(MACOSX_BUNDLE_BUNDLE_VERSION "1")
        set(MACOSX_BUNDLE_COPYRIGHT "Copyright 2026 Cathy J. Fitzpatrick")
        set(MACOSX_BUNDLE_INFO_STRING "")
        set(MACOSX_BUNDLE_ICON_FILE "")
        set(MACOSX_BUNDLE_LONG_VERSION_STRING "")
        configure_file("${CMAKE_ROOT}/Modules/MacOSXBundleInfo.plist.in" "${asr_info}")
    endblock()

    add_executable(asr-image-trace-macos MACOSX_BUNDLE macos_main.cpp)
    target_sources(asr-image-trace-macos PRIVATE FILE_SET CXX_MODULES FILES macos.ixx)
    target_sources(asr-image-trace-macos PRIVATE macos.fish "${asr_info}")
    set_source_files_properties(macos.fish PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # CMake normally generates the bundled Info.plist without a build rule to
    # recreate it after deletion. Copy the configured plist as a bundle resource
    # too, so rebuilding a removed bundle also restores its metadata.
    set_source_files_properties("${asr_info}" PROPERTIES MACOSX_PACKAGE_LOCATION .)
    set_target_properties(asr-image-trace-macos PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
        MACOSX_BUNDLE_INFO_PLIST "${asr_info}"
    )
    target_compile_definitions(asr-image-trace-macos PRIVATE
        FUSE_USE_VERSION=26
        DEVICEFS_ASR_FISH_EXECUTABLE="${FISH_EXECUTABLE}"
        DEVICEFS_ASR_JQ_EXECUTABLE="${JQ_EXECUTABLE}"
    )
    target_link_libraries(asr-image-trace-macos PRIVATE asr-image-trace-core PkgConfig::MACFUSE)
    # Resource copies alone do not relink a bundle. They must also invalidate
    # its signature, which seals the executable, metadata, and script.
    set_property(TARGET asr-image-trace-macos APPEND PROPERTY LINK_DEPENDS
        "${asr_entitlements}"
        "${asr_bundle}/Contents/Info.plist"
        "${asr_bundle}/Contents/Resources/macos.fish"
        "${CMAKE_CURRENT_SOURCE_DIR}/sign-bundle.fish"
    )
    # Signing the bundle signs its main executable and seals its resources.
    # There are no additional nested binaries to sign first; the Fish controller
    # is stored as a resource.
    # https://developer.apple.com/library/archive/technotes/tn2206/
    add_custom_command(TARGET asr-image-trace-macos POST_BUILD
        BYPRODUCTS "${asr_bundle}/Contents/_CodeSignature/CodeResources"
        COMMAND "${FISH_EXECUTABLE}" --no-config --
            "${CMAKE_CURRENT_SOURCE_DIR}/sign-bundle.fish"
            "$<TARGET_BUNDLE_DIR:asr-image-trace-macos>"
            "${CODESIGN_EXECUTABLE}"
            "${DEVICEFS_ASR_SIGNING_IDENTITY}"
            "${asr_entitlements}"
        VERBATIM
    )
endif()
