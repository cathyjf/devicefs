// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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
