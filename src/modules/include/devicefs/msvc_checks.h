// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Stack cookies detect overwrites of protected stack frames. Strict checking
// extends their use to functions that ordinary `/GS` would leave unprotected,
// including functions with local arrays of pointers. The build force-includes
// this header so the setting applies throughout each translation unit.
// https://learn.microsoft.com/en-us/cpp/preprocessor/strict-gs-check
#pragma strict_gs_check(on)
