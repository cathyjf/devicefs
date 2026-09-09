// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The copied width detector expects its build dependencies in `precomp.h`.
// Supplying its integer typedefs and WIL's `LOG_CAUGHT_EXCEPTION` here lets the
// source compile without the rest of Windows Terminal's build.
#include <cstdint>

#include <windows.h>
#include <wil/result.h>

// The width detector reads decoded code points and a fallback buffer only
// after assigning those values. The decoding helpers fill both `cp` locals;
// each fallback branch fills `len` and every `buf` element passed to the callback.
// C26494 nevertheless reports these four declarations because the declarations
// have no initializers. The rule is disabled here so those declarations can
// remain unchanged in Microsoft's source. Only the vendored translation unit
// textually includes this header; first-party code keeps C26494 enabled.
#pragma warning(disable : 26494)
