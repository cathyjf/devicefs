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
#include <roapi.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.h>

#include <wil/resource.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>

export module devicefs.supervisor.find_powershell;

import devicefs.common;

namespace {

// Stable x64 PowerShell's Windows Installer registration.
constexpr auto kPowerShellMsiRegistration =
    L"SOFTWARE\\Microsoft\\PowerShellCore\\InstalledVersions\\"
    L"31ab5147-9a97-4452-8443-d9709f0516e1";
constexpr auto kPowerShellPackageFamily =
    L"Microsoft.PowerShell_8wekyb3d8bbwe";

[[nodiscard]] auto PowerShellPathMSI()
    -> std::optional<std::filesystem::path> {
    auto location_bytes = DWORD{};
    const auto query_error = RegGetValueW(
        HKEY_LOCAL_MACHINE, kPowerShellMsiRegistration, L"InstallLocation",
        RRF_RT_REG_SZ, nullptr, nullptr, &location_bytes);
    if (query_error == ERROR_SUCCESS) {
        auto location = std::wstring(
            location_bytes / sizeof(wchar_t), L'\0');
        const auto read_error = RegGetValueW(
            HKEY_LOCAL_MACHINE, kPowerShellMsiRegistration,
            L"InstallLocation", RRF_RT_REG_SZ, nullptr,
            location.data(), &location_bytes);
        if (read_error != ERROR_SUCCESS) {
            [[gsl::suppress("type.1",
                justification: "Registry APIs return Win32 error codes as LSTATUS.")]]
            const auto win32_error = static_cast<DWORD>(read_error);
            WinError("could not read the PowerShell installation path",
                ExplicitWin32Error{win32_error});
        }
        return std::filesystem::path(location.c_str()) / L"pwsh.exe";
    }
    if (query_error != ERROR_FILE_NOT_FOUND) {
        [[gsl::suppress("type.1",
            justification: "Registry APIs return Win32 error codes as LSTATUS.")]]
        const auto win32_error = static_cast<DWORD>(query_error);
        WinError("could not query the PowerShell installation path",
            ExplicitWin32Error{win32_error});
    }
    return std::nullopt;
}

[[nodiscard]] auto PowerShellPathMSIX() {
    try {
        const auto apartment = wil::RoInitialize();
        const auto manager =
            winrt::Windows::Management::Deployment::PackageManager{};
        const auto packages = manager.FindPackagesWithPackageTypes(
            kPowerShellPackageFamily,
            winrt::Windows::Management::Deployment::PackageTypes::Main);
        if (!packages.First().HasCurrent()) {
            throw std::runtime_error(
                "could not find a registered PowerShell 7 installation");
        }
        const auto sortable_version = [](const auto &package) {
            const auto version = package.Id().Version();
            return std::tuple{
                version.Major, version.Minor, version.Build, version.Revision};
        };
        const auto selected =
            std::ranges::max(packages, {}, sortable_version);
        const auto location = selected.InstalledLocation().Path();
        return std::filesystem::path(location.c_str()) / L"pwsh.exe";
    } catch (const wil::ResultException &error) {
        throw std::runtime_error(std::format(
            "could not initialize the Windows Runtime before finding "
            "PowerShell: {}",
            error.what()));
    } catch (const winrt::hresult_error &error) {
        throw std::runtime_error(std::format(
            "could not query the PowerShell MSIX installation: {}",
            winrt::to_string(error.message())));
    }
}

} // namespace

export [[nodiscard]] auto PowerShellPath() {
    if (const auto path = PowerShellPathMSI()) {
        return *path;
    }
    return PowerShellPathMSIX();
}
