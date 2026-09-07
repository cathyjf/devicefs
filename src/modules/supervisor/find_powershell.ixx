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
#include <appmodel.h>
#include <lmcons.h>
#include <roapi.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.Storage.h>

#include <wil/registry.h>
#include <wil/resource.h>
#include <wil/stl.h>

export module devicefs.supervisor.find_powershell;

import std;
import devicefs.common;
import devicefs.stream_writer;
import devicefs.supervisor.winrt_apartment;

#undef stdout

namespace {

using namespace std::string_view_literals;
using namespace wil::literals;

constexpr auto kPowerShellMsiRegistrationPrefix =
    L"SOFTWARE\\Microsoft\\PowerShellCore\\InstalledVersions\\"sv;
constexpr auto kPowerShellVersionGuids = std::array{
    L"31ab5147-9a97-4452-8443-d9709f0516e1"sv, // x64
    L"1d00683b-0f84-4db8-a64f-2f98ad42fe06"sv, // arm64
};
constexpr auto kPowerShellMsiRegistrationValueName = L"InstallLocation"_zv;
constexpr auto kPowerShellPackageFamily = L"Microsoft.PowerShell_8wekyb3d8bbwe"_zv;

[[nodiscard]] auto PowerShellPathMsi(const auto version_guid)
    -> std::optional<std::filesystem::path> {
    const auto subkey_name = std::format(L"{}{}",
        kPowerShellMsiRegistrationPrefix, version_guid);
    auto location = wil::unique_cotaskmem_string{};
    const auto result = wil::reg::get_value_string_nothrow(
        HKEY_LOCAL_MACHINE, subkey_name.c_str(),
        kPowerShellMsiRegistrationValueName.c_str(), location);
    if (wil::reg::is_registry_not_found(result)) {
        return std::nullopt;
    } else if (FAILED(result)) {
        WinError(
            "error while querying the PowerShell installation path: "
            "HKLM\\{}\\{}",
            std::wstring_view{subkey_name},
            std::wstring_view{kPowerShellMsiRegistrationValueName},
            ExplicitWin32Error::FromHresult(result));
    }
    return std::filesystem::path(location.get()) / L"pwsh.exe";
}

[[nodiscard]] auto PowerShellPathMsix()
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

export constexpr auto kRegisterMsixOption = "--internal-register-msix"sv;

export auto EnsurePowerShellMsixRegistration(const wil::zwstring_view package_full_name) {
    const auto family_name = [package_full_name] {
        auto family_name_length = UINT32{};
        if (const auto error = PackageFamilyNameFromFullName(
                package_full_name.c_str(), &family_name_length, nullptr);
            error != ERROR_INSUFFICIENT_BUFFER) {
            WinError("could not determine the package family name length for '{}'",
                std::wstring_view{package_full_name},
                ExplicitWin32Error{std::bit_cast<DWORD>(error)});
        }
        // The reported length includes the terminator, which `std::wstring`
        // supplies in addition to its characters.
        auto result = std::wstring(family_name_length - 1, L'\0');
        if (const auto error = PackageFamilyNameFromFullName(
                package_full_name.c_str(), &family_name_length, result.data());
            error != ERROR_SUCCESS) {
            WinError("could not determine the package family name for '{}'",
                std::wstring_view{package_full_name},
                ExplicitWin32Error{std::bit_cast<DWORD>(error)});
        }
        return result;
    }();

    // This command registers the caller's chosen PowerShell package for the
    // current user. The family check limits registration to PowerShell; the
    // caller remains responsible for selecting a trusted package. Registering
    // by full name preserves the chosen package's exact version and architecture.
    if (std::wstring_view{family_name} != kPowerShellPackageFamily) {
        throw std::invalid_argument(std::format(
            "MSIX package '{}' does not belong to the allowed PowerShell family '{}'",
            winrt::to_string(package_full_name),
            winrt::to_string(kPowerShellPackageFamily)));
    }

    const auto apartment = WinrtApartment{
        "could not initialize WinRT to register PowerShell", RO_INIT_MULTITHREADED};
    const auto user_name = [] {
        auto buffer = std::array<char, UNLEN + 1>{};
        auto length = CompileTimeCast<DWORD, buffer.size()>();
        if (!GetUserNameA(buffer.data(), &length)) {
            WinError("could not obtain the current user name");
        }
        return std::string{buffer.data(), length - 1};
    }();
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: ensuring MSIX package '{}' is registered for user '{}'\n",
        winrt::to_string(package_full_name), user_name);
    try {
        const auto manager =
            winrt::Windows::Management::Deployment::PackageManager{};
        const auto deployment = manager.RegisterPackageByFullNameAsync(
            package_full_name, nullptr,
            winrt::Windows::Management::Deployment::DeploymentOptions::None).get();
        if (const auto result = deployment.ExtendedErrorCode(); FAILED(result)) {
            throw winrt::hresult_error{result, deployment.ErrorText()};
        }
    } catch (const winrt::hresult_error &error) {
        WinError("could not register MSIX package '{}': {}",
            std::wstring_view{package_full_name}, std::wstring_view{error.message()},
            ExplicitWin32Error::FromHresult(error.code()));
    }
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: MSIX package '{}' is registered for user '{}'\n",
        winrt::to_string(package_full_name), user_name);
}

export [[nodiscard]] auto PowerShellPath()
    -> std::optional<std::filesystem::path> {
    for (const auto guid : kPowerShellVersionGuids) {
        if (const auto path = PowerShellPathMsi(guid)) {
            return path;
        }
    }
    return PowerShellPathMsix();
}
