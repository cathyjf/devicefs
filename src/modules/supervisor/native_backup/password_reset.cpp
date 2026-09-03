// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

module;

#include <windows.h>
#include <bcrypt.h>
#include <lm.h>

#include <wil/resource.h>
#include <wil/safecast.h>
#include <wil/stl.h>

#undef stderr
#undef stdout

module devicefs.supervisor.native_backup:password_reset;

import std;
import devicefs.common;

namespace internal {

[[nodiscard]] auto ResetBackupAccountPassword(
    const wil::zwstring_view username) -> wil::secure_wstring {
    auto random = std::array<unsigned char, 32>{};
    const auto erase_random =
        wil::SecureZeroMemory_scope_exit(random.data(), random.size());
    const auto status = BCryptGenRandom(
        nullptr, random.data(),
        wil::safe_cast_failfast<ULONG>(random.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        throw std::runtime_error(std::format(
            "could not generate the backup account password "
            "(NTSTATUS 0x{:08x})",
            std::bit_cast<std::uint32_t>(status)));
    }

    constexpr auto digits = wil::zwstring_view{L"0123456789abcdef"};
    auto password = wil::secure_wstring(random.size() * 2, L'\0');
    for (auto index = 0uz; index < random.size(); ++index) {
        password[index * 2] = digits.at(random.at(index) >> 4);
        password[index * 2 + 1] = digits.at(random.at(index) & 0x0f);
    }

    auto account = USER_INFO_1003{
        .usri1003_password = password.data(),
    };
    [[gsl::suppress("26490",
        justification:
            "`NetUserSetInfo` receives the structure through its generic "
            "`BYTE *` buffer parameter. The value 1003 tells it to interpret "
            "that buffer as a `USER_INFO_1003` containing the replacement "
            "password.")]]
    const auto error = NetUserSetInfo(
        nullptr, username.c_str(), 1003,
        reinterpret_cast<BYTE *>(&account), nullptr);
    if (error != NERR_Success) {
        WinError("could not reset the password for backup account '{}'",
            std::wstring_view{username.c_str(), username.size()},
            ExplicitWin32Error{error});
    }
    return password;
}

} // namespace internal
