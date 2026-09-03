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

#include <wil/registry.h>
#include <wil/resource.h>

export module devicefs.supervisor.find_powershell;

import std;
import devicefs.common;

namespace {

constexpr auto kPowerShellMsiRegistration =
    L"SOFTWARE\\Microsoft\\PowerShellCore\\InstalledVersions\\"
    L"31ab5147-9a97-4452-8443-d9709f0516e1";
constexpr auto kPowerShellMsiRegistrationValueName = L"InstallLocation";
constexpr auto kPowerShellPackageFamily = L"Microsoft.PowerShell_8wekyb3d8bbwe";

[[nodiscard]] auto PowerShellPathMSI()
    -> std::optional<std::filesystem::path> {
    auto location = wil::unique_cotaskmem_string{};
    const auto result = wil::reg::get_value_string_nothrow(
        HKEY_LOCAL_MACHINE, kPowerShellMsiRegistration,
        kPowerShellMsiRegistrationValueName, location);
    if (wil::reg::is_registry_not_found(result)) {
        return std::nullopt;
    } else if (FAILED(result)) {
        WinError(
            "error while querying the PowerShell installation path: "
            "HKLM\\{}\\{}",
            std::wstring_view{kPowerShellMsiRegistration},
            std::wstring_view{kPowerShellMsiRegistrationValueName},
            ExplicitWin32Error::FromHresult(result));
    }
    return std::filesystem::path(location.get()) / L"pwsh.exe";
}

[[nodiscard]] auto PowerShellPathMSIX()
    -> std::optional<std::filesystem::path> {
    try {
        const auto apartment = wil::RoInitialize();
        const auto manager =
            winrt::Windows::Management::Deployment::PackageManager{};
        // This query returns packages registered by any user. An unprivileged
        // user can register a package signed by a certificate trusted only by
        // that user and give it this package family because a family name
        // incorporates the certificate subject rather than its public key.
        // However, a package trusted only by such a user is not classified as
        // Store-signed, so this filter excludes it from the PowerShell
        // installation candidates. See
        // <https://learn.microsoft.com/en-us/uwp/api/windows.management.deployment.packagemanager.findpackageswithpackagetypes>,
        // <https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/package-identity-overview>,
        // and <https://learn.microsoft.com/en-us/uwp/api/windows.applicationmodel.packagesignaturekind>.
        auto packages = manager.FindPackagesWithPackageTypes(
            kPowerShellPackageFamily,
            winrt::Windows::Management::Deployment::PackageTypes::Main) |
            std::views::filter([](const auto &package) {
                return package.SignatureKind() ==
                    winrt::Windows::ApplicationModel::PackageSignatureKind::Store;
            });
        if (packages.begin() == packages.end()) {
            return std::nullopt;
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

export [[nodiscard]] auto PowerShellPath()
    -> std::optional<std::filesystem::path> {
    if (const auto path = PowerShellPathMSI()) {
        return path;
    }
    return PowerShellPathMSIX();
}
