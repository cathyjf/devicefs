// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

import std;

// The converter and width detector share these types so converted text can
// pass directly between them. UTF-16 uses `wchar_t` when it has the same size
// as `char16_t`, and otherwise uses `char16_t`. UTF-32 makes the corresponding
// choice for `char32_t`. The assertions require the selected types to represent
// every code unit needed by their encoding.
// This header is shared by module interfaces, a header unit, and ordinary
// translation units. C++ linkage keeps the declarations in the global module.
extern "C++" {
namespace devicefs::terminal {

using utf16_code_unit = std::conditional_t<sizeof(wchar_t) == sizeof(char16_t), wchar_t, char16_t>;
using utf32_code_unit = std::conditional_t<sizeof(wchar_t) == sizeof(char32_t), wchar_t, char32_t>;

static_assert((sizeof(wchar_t) == 2 || sizeof(wchar_t) == 4) &&
    std::numeric_limits<utf16_code_unit>::max() >= 0xffff &&
    std::numeric_limits<utf32_code_unit>::max() >= 0x10ffff,
    "The Unicode adapters require wchar_t to contain UTF-16 or UTF-32 code units");

}
}
