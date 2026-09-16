# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

include_guard(DIRECTORY)

set(DEVICEFS_PRODUCT_VERSION "0.1")
set(DEVICEFS_PRODUCT_VERSION_HEADER "${CMAKE_CURRENT_BINARY_DIR}/versioninfo/product-version.h")

# Run on every build so a new commit is detected without reconfiguring CMake.
# The generator writes the header only when its contents change, avoiding
# resource recompilation and relinking when the revision is unchanged.
add_custom_target(devicefs-product-version
    COMMAND "${CMAKE_COMMAND}"
        "-DVERSION=${DEVICEFS_PRODUCT_VERSION}"
        "-DOUTPUT=${DEVICEFS_PRODUCT_VERSION_HEADER}"
        -P "${CMAKE_CURRENT_LIST_DIR}/GenerateProductVersion.cmake"
    BYPRODUCTS "${DEVICEFS_PRODUCT_VERSION_HEADER}"
    VERBATIM
)

function(devicefs_version_info target description file_version)
    string(REPLACE "." "," file_version_numbers "${file_version}")
    string(REPLACE "." "," product_version_numbers "${DEVICEFS_PRODUCT_VERSION}")
    set(filename "$<TARGET_FILE_NAME:${target}>")
    set(debug_flag "$<IF:$<CONFIG:Debug>,VS_FF_DEBUG,0>")
    set(template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/resources/versioninfo.rc.in")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${template}")
    file(READ "${template}" resource)
    string(CONFIGURE "${resource}" resource @ONLY)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/versioninfo/$<CONFIG>/${target}.rc")
    file(GENERATE OUTPUT "${output}" CONTENT "${resource}")
    target_sources(${target} PRIVATE "${output}")
    add_dependencies(${target} devicefs-product-version)
endfunction()
