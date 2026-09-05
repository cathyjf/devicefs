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
#include <DismApi.h>
#include <lm.h>
#include <objbase.h>

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

[[nodiscard]] auto InstallWslPackage() -> bool;

namespace {

constexpr auto kWslRegistration = wil::zwstring_view{
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Lxss\\MSI"};

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

[[nodiscard]] auto IsSuitableWslPackageInstalled() -> bool {
    // The package version check compares numeric components and treats omitted
    // trailing components as zero, so `2.7.12` meets a minimum of `2.7.12.0`.
    // However, `std::ranges::lexicographical_compare` considers a matching
    // shorter sequence smaller. Omitting trailing zeros from `minimum_version`
    // prevents that comparison from rejecting an equivalent shorter version.
    constexpr auto minimum_version = std::array{2u, 7u, 13u};
    auto version = wil::unique_cotaskmem_string{};
    if (const auto result = wil::reg::get_value_string_nothrow(
            HKEY_LOCAL_MACHINE, kWslRegistration.c_str(), L"Version", version);
        wil::reg::is_registry_not_found(result)) {
        return false;
    } else if (FAILED(result)) {
        WinError("could not read the WSL package version from 'HKLM\\{}'",
            std::wstring_view{kWslRegistration},
            ExplicitWin32Error::FromHresult(result));
    }

    return !std::ranges::lexicographical_compare(
        std::wstring_view{version.get()} | std::views::split(L'.') |
            std::views::transform([](const auto component) {
                return std::stoul(std::wstring{component.begin(), component.end()});
            }),
        minimum_version);
}

[[nodiscard]] auto EnsureWsl1Component() -> bool {
    constexpr auto feature =
        wil::zwstring_view{L"Microsoft-Windows-Subsystem-Linux"};
    if (const auto result =
            DismInitialize(DismLogErrorsWarnings, nullptr, nullptr);
        FAILED(result)) {
        WinError("could not initialize DISM to prepare Windows component '{}'",
            std::wstring_view{feature},
            ExplicitWin32Error::FromHresult(result));
    }
    const auto shutdown =
        wil::unique_call<decltype(&DismShutdown), DismShutdown>{};
    auto session = wil::unique_any<
        DismSession, decltype(&DismCloseSession), DismCloseSession>{};
    if (const auto result = DismOpenSession(
            DISM_ONLINE_IMAGE, nullptr, nullptr, session.addressof());
        FAILED(result)) {
        WinError("could not open the running Windows installation in DISM",
            ExplicitWin32Error::FromHresult(result));
    }

    auto information = std::unique_ptr<DismFeatureInfo,
        wil::function_deleter<decltype(&DismDelete), DismDelete>>{};
    if (const auto result = DismGetFeatureInfo(
            session.get(), feature.c_str(), nullptr,
            DismPackageNone, wil::out_param(information));
        FAILED(result)) {
        WinError("could not query Windows component '{}'",
            std::wstring_view{feature},
            ExplicitWin32Error::FromHresult(result));
    }
    if (information->FeatureState == DismStateInstalled) {
        devicefs::WriteToStream(
            devicefs::stdout,
            L"backup-supervisor: Windows component '{}' is already installed\n",
            std::wstring_view{feature});
        return false;
    }
    if (information->FeatureState == DismStateInstallPending) {
        devicefs::WriteToStream(
            devicefs::stdout,
            L"backup-supervisor: Windows component '{}' is awaiting a restart "
            L"to complete installation\n",
            std::wstring_view{feature});
        return true;
    }

    devicefs::WriteToStream(
        devicefs::stdout,
        L"backup-supervisor: installing Windows component '{}'\n",
        std::wstring_view{feature});
    const auto result = DismEnableFeature(
        session.get(), feature.c_str(), nullptr, DismPackageNone, FALSE,
        nullptr, 0, TRUE, nullptr, nullptr, nullptr);
    if (FAILED(result)) {
        WinError("could not install Windows component '{}'",
            std::wstring_view{feature},
            ExplicitWin32Error::FromHresult(result));
    }
    devicefs::WriteToStream(
        devicefs::stdout,
        L"backup-supervisor: installed Windows component '{}'\n",
        std::wstring_view{feature});
    if (result == ERROR_SUCCESS_REBOOT_REQUIRED) {
        devicefs::WriteToStream(
            devicefs::stdout,
            L"backup-supervisor: Windows must be restarted to complete "
            L"installation of '{}'\n",
            std::wstring_view{feature});
        return true;
    }
    return false;
}

} // namespace

export [[nodiscard]] auto WslExecutablePath() {
    auto location = wil::unique_cotaskmem_string{};
    if (const auto result = wil::reg::get_value_string_nothrow(
            HKEY_LOCAL_MACHINE, kWslRegistration.c_str(), L"InstallLocation", location);
        FAILED(result)) {
        WinError("could not read the WSL installation location from 'HKLM\\{}'",
            std::wstring_view{kWslRegistration},
            ExplicitWin32Error::FromHresult(result));
    }
    return std::filesystem::path{location.get()} / L"wsl.exe";
}

export auto EnsureInternalWindowsAccount(const wil::zwstring_view username) {
    if (CreateAccountIfMissing(username)) {
        devicefs::WriteToStream(
            devicefs::stdout,
            L"backup-supervisor: created internal Windows account '{}'\n",
            std::wstring_view{username.c_str(), username.size()});
    }
    HideAccountFromLogonScreen(username);
    const auto package_restart_needed = [] {
        if (IsSuitableWslPackageInstalled()) {
            devicefs::WriteToStream(
                devicefs::stdout,
                "backup-supervisor: a suitable version of `wsl.exe` is installed\n");
            return false;
        }
        return InstallWslPackage();
    }();
    if (EnsureWsl1Component() || package_restart_needed) {
        devicefs::WriteToStream(
            devicefs::stdout,
            "The installation is not complete. After restarting the computer, "
            "please run `backup-supervisor.exe --install` again to complete "
            "the installation.\n");
    }
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
