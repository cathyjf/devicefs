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

#include <wil/registry.h>
#include <wil/resource.h>
#include <wil/safecast.h>
#include <wil/stl.h>

#undef stderr
#undef stdout

export module devicefs.supervisor.account_management;

import std;
import devicefs.common;
import devicefs.stream_writer;

namespace {

[[nodiscard]] auto GenerateAccountPassword() -> wil::secure_wstring {
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
    return password;
}

[[nodiscard]] auto CreateAccountIfMissing(const wil::zwstring_view username) {
    auto name = std::wstring{username.c_str(), username.size()};
    auto password = GenerateAccountPassword();
    auto account = USER_INFO_1{
        .usri1_name = name.data(),
        .usri1_password = password.data(),
        .usri1_priv = USER_PRIV_USER,
        .usri1_flags = UF_SCRIPT | UF_NORMAL_ACCOUNT,
    };
    [[gsl::suppress("26490",
        justification:
            "`NetUserAdd` receives the structure through its generic "
            "`BYTE *` buffer parameter. The value 1 tells it to interpret "
            "that buffer as a `USER_INFO_1` containing the new account's "
            "name, initial password, and account settings.")]]
    const auto error = NetUserAdd(
        nullptr, 1, reinterpret_cast<BYTE *>(&account), nullptr);
    if (error == NERR_UserExists) {
        return false;
    }
    if (error != NERR_Success) {
        WinError("could not create internal Windows account '{}'",
            std::wstring_view{username.c_str(), username.size()},
            ExplicitWin32Error{error});
    }
    return true;
}

auto HideAccountFromLogonScreen(const wil::zwstring_view username) {
    const auto result = wil::reg::set_value_dword_nothrow(
        HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon\\"
        L"SpecialAccounts\\UserList",
        username.c_str(), 0);
    if (FAILED(result)) {
        devicefs::WriteToStream(
            devicefs::stderr,
            L"backup-supervisor: could not hide internal Windows account '{}' "
            L"from the logon screen (Windows error 0x{:08x})\n",
            std::wstring_view{username.c_str(), username.size()},
            ExplicitWin32Error::FromHresult(result).value);
    }
}

} // namespace

export auto EnsureInternalWindowsAccount(const wil::zwstring_view username) {
    if (CreateAccountIfMissing(username)) {
        devicefs::WriteToStream(
            devicefs::stdout,
            L"backup-supervisor: created internal Windows account '{}'\n",
            std::wstring_view{username.c_str(), username.size()});
    }
    HideAccountFromLogonScreen(username);
}

export [[nodiscard]] auto ResetBackupAccountPassword(
    const wil::zwstring_view username) -> wil::secure_wstring {
    auto information = std::unique_ptr<USER_INFO_1,
        wil::function_deleter<decltype(&NetApiBufferFree), NetApiBufferFree>>{};
    const auto query_error = NetUserGetInfo(
        nullptr, username.c_str(), 1, wil::out_param_ptr<BYTE **>(information));
    if (query_error != NERR_Success) {
        WinError("could not query privileges for backup account '{}'",
            std::wstring_view{username.c_str(), username.size()},
            ExplicitWin32Error{query_error});
    }
    if (information->usri1_priv == USER_PRIV_ADMIN) {
        throw std::runtime_error(std::format(
            "refusing to reset the password for backup account '{}' "
            "because it is an administrator",
            std::filesystem::path{username.c_str()}.string()));
    }

    auto password = GenerateAccountPassword();
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
