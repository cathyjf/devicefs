# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

set(DEVICEFS_MSVC_FLAGS /EHsc /utf-8 /guard:cf /guard:ehcont)

# These settings retain `/sdl`'s diagnostics and strict stack-cookie checks.
# In the MSVC ARM64 build, `/sdl` additionally cleared entire `TranscodedText`
# objects, including unused character storage. That observed clearing is not
# explained by its documented pointer-member initialization.
# https://learn.microsoft.com/en-us/cpp/build/reference/sdl-enable-additional-security-checks
list(APPEND DEVICEFS_MSVC_FLAGS
    /sdl- /GS
    /we4146 /we4308 /we4532 /we4533 /we4700 /we4703 /we4789 /we4995 /we4996
    "/FI${CMAKE_CURRENT_LIST_DIR}/../src/modules/include/devicefs/msvc_checks.h"
)

# C5260 warns that importing a header unit changes a constant's linkage.
# WIL's `__buffer_size` supplies a buffer size; its object identity is irrelevant
# to that use, so the linkage change does not require modifying WIL.
# https://learn.microsoft.com/en-us/cpp/error-messages/compiler-warnings/compiler-warnings-c5200-through-c5399
list(APPEND DEVICEFS_MSVC_FLAGS /wd5260)
