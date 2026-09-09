// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// This file contains definitions that allow the Windows Terminal width
// detector to compile as part of this project.

#pragma once

#include <cstdint>

#ifndef _MSC_VER
    #include <cwchar>

    #ifndef __has_builtin
        #define __has_builtin(x) 0
    #endif

    #if __has_builtin(__builtin_assume)
        #define __assume(condition) __builtin_assume(condition)
    #elif __has_cpp_attribute(assume)
        #define __assume(condition) [[assume(condition)]]
    #endif
#endif

#define LOG_CAUGHT_EXCEPTION()

#ifdef _MSC_VER
    // The width detector reads decoded code points and a fallback buffer only
    // after assigning those values. The decoding helpers fill both `cp` locals;
    // each fallback branch fills `len` and every `buf` element passed to the callback.
    // C26494 nevertheless reports these four declarations because the declarations
    // have no initializers. The rule is disabled here so those declarations can
    // remain unchanged in Microsoft's source. Only the vendored translation unit
    // textually includes this header; first-party code keeps C26494 enabled.
    #pragma warning(disable : 26494)
#endif
