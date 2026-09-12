// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#ifdef _MSC_VER
    #define ATTRIBUTE_FORCEINLINE [[msvc::forceinline]]
#elifdef __GNUC__
    #define ATTRIBUTE_FORCEINLINE __attribute__((always_inline))
#endif

#ifdef _MSC_VER
    #define ATTRIBUTE_MSVC_FLATTEN [[msvc::flatten]]
#else
    #define ATTRIBUTE_MSVC_FLATTEN
#endif
