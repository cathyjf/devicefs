// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// `GSL_SUPPRESS` suppresses a named analyzer rule while keeping the reason
// beside the code. MSVC records the justification in the attribute; Clang
// accepts only the rule name.
#ifdef _MSC_VER
#define GSL_SUPPRESS(rule, rationale) [[gsl::suppress(rule, justification: rationale)]]
#else
#define GSL_SUPPRESS(rule, rationale) [[gsl::suppress(rule)]]
#endif
