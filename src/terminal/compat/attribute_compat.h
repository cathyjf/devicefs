// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef _MSC_VER
    #define ATTRIBUTE_FORCEINLINE [[msvc::forceinline]]
#elifdef __GNUC__
    #define ATTRIBUTE_FORCEINLINE __attribute__((always_inline))
#else
    #define ATTRIBUTE_FORCEINLINE
#endif

#ifdef _MSC_VER
    #define ATTRIBUTE_MSVC_FLATTEN [[msvc::flatten]]
#else
    #define ATTRIBUTE_MSVC_FLATTEN
#endif

#if defined(_MSC_VER) && (_MSC_VER >= 1929)
    #define ATTRIBUTE_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#elif __has_cpp_attribute(no_unique_address) >= 201803L
    #define ATTRIBUTE_NO_UNIQUE_ADDRESS [[no_unique_address]]
#else
    #define ATTRIBUTE_NO_UNIQUE_ADDRESS
#endif

// `GSL_SUPPRESS` suppresses a named analyzer rule while keeping the reason
// beside the code. MSVC records the justification in the attribute; Clang
// accepts only the rule name. GCC does not support the attribute at all.
#ifdef _MSC_VER
    #define GSL_SUPPRESS(rule, rationale) [[gsl::suppress(rule, justification: rationale)]]
#elifdef __clang__
    #define GSL_SUPPRESS(rule, rationale) [[gsl::suppress(rule)]]
#else
    #define GSL_SUPPRESS(rule, rationale)
#endif
