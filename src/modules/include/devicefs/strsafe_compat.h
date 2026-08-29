// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// As a result of an apparent compiler defect, when certain WIL headers are
// imported (instead of included), MSVC++ is able to find the declaration of
// `StringValidateDestW` but not the definition of it, even though both the
// declaration and definition are contained within `strsafe.h` and the compiler
// should presumably either find both or neither.
//
// Include this compatibility header only in translation units where the
// supported compiler reproduces the defect.
#include <strsafe.h>

// Remove unwanted macros transitively included by the above header.
#undef stderr
#undef stdout
