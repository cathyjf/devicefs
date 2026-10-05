# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# CTest's libuv launcher sets `STARTF_USESHOWWINDOW`, which makes PowerShell
# attempt taskbar jump-list initialization even in a headless console. That
# initialization can crash the CLR. Launching PowerShell directly through
# `conhost` omits the flag and therefore skips the failing COM operations.
# PowerShell's startup-info check:
# https://github.com/PowerShell/PowerShell/blob/v7.6.5/src/Microsoft.PowerShell.ConsoleHost/WindowsTaskbarJumpList/TaskbarJumpList.cs#L51-L71
# Libuv's startup flags:
# https://github.com/Kitware/CMake/blob/v4.2.3/Utilities/cmlibuv/src/win/process.c#L1016-L1054

# `conhost` does not forward its child's exit status. A separate result file
# lets CTest distinguish a successful test from a failed or crashed child;
# the random suffix also separates concurrent invocations of the same test.
string(RANDOM LENGTH 32 ALPHABET "0123456789abcdef" result_id)
set(result_file "${CMAKE_CURRENT_BINARY_DIR}/native-test-${INDEX}-${result_id}.exit")
execute_process(
    COMMAND conhost.exe --headless "${POWERSHELL_EXECUTABLE}"
        -NoProfile -NonInteractive
        -File "${CMAKE_CURRENT_LIST_DIR}/native_windows.ps1"
        -Executable "${TEST_EXECUTABLE}"
        -Index "${INDEX}"
        -ResultFile "${result_file}"
    RESULT_VARIABLE conhost_result
)
if(NOT EXISTS "${result_file}")
    message(FATAL_ERROR
        "Native terminal test ${INDEX} did not report its exit status "
        "(conhost result: ${conhost_result}).")
endif()
file(READ "${result_file}" exit_code)
file(REMOVE "${result_file}")
string(STRIP "${exit_code}" exit_code)
if(NOT exit_code STREQUAL "0")
    message(FATAL_ERROR
        "Native terminal test ${INDEX} exited with code ${exit_code}.")
endif()
