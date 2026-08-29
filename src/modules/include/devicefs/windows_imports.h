// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// This header unit centralizes the relatively expensive compilation of the
// Windows/WIL transitive textual inclusion graph. Build measurement showed
// that introduction of this header unit reduced the time for a clean project
// build from about 50 seconds to about 40 seconds.
//
// Additional #include directives should not be added below without concrete
// measurement-backed performance justification. Prospective new additions
// must be weighed against the maintenance downsides of the client importers
// no longer expressing their dependencies as precisely.
//
// No additional header unit wrapper files should be created without an
// individualized evidence-based rationale.

#pragma once

#include <windows.h>
#include <roapi.h>
#include <userenv.h>

#include <wil/resource.h>
#include <wil/result.h>
#include <wil/safecast.h>
#include <wil/stl.h>
#include <wil/token_helpers.h>
#include <wil/win32_helpers.h>

#include <winrt/base.h>

// Remove unwanted macros transitively included by the above headers.
#undef stderr
#undef stdout
