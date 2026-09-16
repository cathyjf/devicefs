# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/.."
        OUTPUT_VARIABLE revision
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE result
        ERROR_QUIET
    )
    if(result EQUAL 0)
        string(APPEND VERSION "-${revision}")
    endif()
endif()

set(content "#define DEVICEFS_PRODUCT_VERSION_TEXT \"${VERSION}\"\n")
if(EXISTS "${OUTPUT}")
    file(READ "${OUTPUT}" previous)
    if(content STREQUAL previous)
        return()
    endif()
endif()
file(WRITE "${OUTPUT}" "${content}")
