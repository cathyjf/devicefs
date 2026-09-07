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
#include <wil/win32_helpers.h>

export module devicefs.supervisor.launch_powershell;

import std;
import devicefs.common;
import devicefs.stream_writer;
import devicefs.supervisor.account_management;
import devicefs.supervisor.installation;
import devicefs.supervisor.winrt_apartment;

#undef stderr
#undef stdout

using namespace std::string_literals;
using namespace std::string_view_literals;

export constexpr auto kRegisterMsixOption = "--internal-register-msix"sv;
export constexpr auto kPreparePowerShellProfileOption = "--internal-prepare-powershell-profile"sv;

namespace {

using namespace wil::literals;

constexpr auto kPowerShellMsiRegistrationPrefix =
    L"SOFTWARE\\Microsoft\\PowerShellCore\\InstalledVersions\\"sv;
constexpr auto kPowerShellVersionGuids = std::array{
    L"31ab5147-9a97-4452-8443-d9709f0516e1"sv, // x64
    L"1d00683b-0f84-4db8-a64f-2f98ad42fe06"sv, // arm64
};
constexpr auto kPowerShellMsiRegistrationValueName = L"InstallLocation"_zv;
constexpr auto kPowerShellPackageFamily = L"Microsoft.PowerShell_8wekyb3d8bbwe"_zv;
constexpr auto kTerminalPackageFamily = L"Microsoft.WindowsTerminal_8wekyb3d8bbwe"_zv;

constexpr auto kPowerShellProfile = R"(
function prompt {
    "PS {0} {1}{2} " -f (
        [System.Environment]::UserName,
        $executionContext.SessionState.Path.CurrentLocation,
        ('>' * ($nestedPromptLevel + 1))
    )
}
)"sv;

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

[[nodiscard]] auto TryRunConsolePreparation(
    const wil::zwstring_view username,
    const std::string_view description,
    const auto &...arguments) -> bool {
    const auto supervisor = InstalledExecutablePath();
    try {
        auto command = std::filesystem::path{wil::ArgvToCommandLine(
            std::array{supervisor.string(), arguments...})}.wstring();
        if (const auto exit_code = RunInternalWindowsAccountProcess(
                username, supervisor, std::move(command));
            exit_code != 0) {
            devicefs::WriteToStream(devicefs::stderr,
                "backup-supervisor: {} for user '{}' failed with exit code "
                "0x{:08x}; continuing console launch\n",
                description, winrt::to_string(username), exit_code);
            return false;
        }
    } catch (const std::system_error &error) {
        if (error.code().category() != std::system_category()) {
            throw;
        }
        devicefs::WriteToStream(devicefs::stderr,
            "backup-supervisor: {} for user '{}' failed "
            "(Windows error 0x{:08x}): {}; continuing console launch\n",
            description, winrt::to_string(username),
            std::bit_cast<DWORD>(error.code().value()), error.what());
        if (error.code().value() == ERROR_FILE_NOT_FOUND) {
            devicefs::WriteToStream(devicefs::stderr,
                L"The backup supervisor may not be installed at '{}'.\n"
                L"Use the `--install` command to install it.\n",
                supervisor.native());
        }
        return false;
    }
    return true;
}

[[nodiscard]] auto FindAndPrepareMsixApplication(
    const wil::zwstring_view username,
    const wil::zwstring_view package_family,
    const std::wstring_view executable)
    -> std::optional<std::filesystem::path> {
    const auto apartment = WinrtApartment{
        "could not initialize WinRT to find a console package", RO_INIT_MULTITHREADED};
    const auto manager =
        winrt::Windows::Management::Deployment::PackageManager{};
    // This query returns packages registered by any user. An unprivileged
    // user can register a package signed by a certificate trusted only by
    // that user and give it this package family because a family name
    // incorporates the certificate subject rather than its public key.
    // However, a package trusted only by such a user is not classified as
    // Store-signed, so this filter excludes it from the application
    // installation candidates. See
    // <https://learn.microsoft.com/en-us/uwp/api/windows.management.deployment.packagemanager.findpackageswithpackagetypes>,
    // <https://learn.microsoft.com/en-us/windows/apps/desktop/modernize/package-identity-overview>,
    // and <https://learn.microsoft.com/en-us/uwp/api/windows.applicationmodel.packagesignaturekind>.
    auto packages = manager.FindPackagesWithPackageTypes(
        package_family,
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
    // The package files are shared across users, but Windows requires the
    // backup account to register the package before it can execute its programs.
    // Register the exact Store-signed version selected above by running the
    // installed supervisor as that account, and wait for it to finish before
    // returning the executable to the interactive-console launcher.
    if (!TryRunConsolePreparation(username,
            std::format("MSIX registration of '{}'", winrt::to_string(package_family)),
            std::string{kRegisterMsixOption},
            winrt::to_string(selected.Id().FullName()))) {
        return std::nullopt;
    }
    return std::filesystem::path(location.c_str()) / executable;
}

[[nodiscard]] auto TryFindAndPrepareApplication(
    const std::string_view application, const auto &prepare)
    -> std::optional<std::filesystem::path> {
    try {
        return std::invoke(prepare);
    } catch (const winrt::hresult_error &error) {
        devicefs::WriteToStream(devicefs::stderr,
            "backup-supervisor: could not query the {} MSIX installation "
            "(error 0x{:08x}): {}; trying the next console option\n",
            application,
            ExplicitWin32Error::FromHresult(error.code()).value,
            winrt::to_string(error.message()));
        return std::nullopt;
    }
}

[[nodiscard]] auto FindAndPreparePowerShell(const wil::zwstring_view username)
    -> std::optional<std::filesystem::path> {
    for (const auto guid : kPowerShellVersionGuids) {
        if (const auto path = PowerShellPathMsi(guid)) {
            return path;
        }
    }
    return TryFindAndPrepareApplication("PowerShell"sv, [username] {
        return FindAndPrepareMsixApplication(
            username, kPowerShellPackageFamily, L"pwsh.exe"sv);
    });
}

[[nodiscard]] auto FindAndPrepareWindowsTerminal(const wil::zwstring_view username)
    -> std::optional<std::filesystem::path> {
    return TryFindAndPrepareApplication("Windows Terminal"sv, [username] {
        // Terminal's package manifest declares `wt.exe` as its command-line
        // entry point, so use that launcher from the selected package. See
        // <https://github.com/microsoft/terminal/blob/main/src/cascadia/CascadiaPackage/Package.appxmanifest>.
        return FindAndPrepareMsixApplication(
            username, kTerminalPackageFamily, L"wt.exe"sv);
    });
}

} // namespace

export auto EnsurePowerShellProfile() {
    // This mode runs as the backup account with its Windows profile loaded by
    // `RunInternalWindowsAccountProcess`. Resolve that account's Documents folder
    // because PowerShell follows its configured location, including redirection.
    const auto directory = DocumentsDirectory() / L"PowerShell";
    std::filesystem::create_directories(directory);
    const auto path = directory / L"Profile.ps1";
    auto profile = std::ofstream{path, std::ios::binary | std::ios::noreplace};
    if (!profile && std::filesystem::exists(path)) {
        return;
    }
    std::println(profile, "{}", kPowerShellProfile);
    profile.flush();
    if (!profile) {
        throw std::runtime_error(std::format(
            "could not write the PowerShell profile '{}'", path.string()));
    }
    devicefs::WriteToStream(devicefs::stdout,
        "backup-supervisor: created PowerShell profile '{}'\n", path.string());
}

export auto EnsureConsoleMsixRegistration(const wil::zwstring_view package_full_name) {
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

    // This command registers the caller's chosen console package for the current
    // user. The family check limits registration to PowerShell and Windows
    // Terminal; the caller remains responsible for selecting a trusted package.
    // Registering by full name preserves its exact version and architecture.
    if ((std::wstring_view{family_name} != kPowerShellPackageFamily) &&
        (std::wstring_view{family_name} != kTerminalPackageFamily)) {
        throw std::invalid_argument(std::format(
            "MSIX package '{}' does not belong to the allowed PowerShell family '{}' "
            "or Windows Terminal family '{}'",
            winrt::to_string(package_full_name),
            winrt::to_string(kPowerShellPackageFamily),
            winrt::to_string(kTerminalPackageFamily)));
    }

    const auto apartment = WinrtApartment{
        "could not initialize WinRT to register a console package", RO_INIT_MULTITHREADED};
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

export [[nodiscard]] auto LaunchPowerShell(const wil::zwstring_view username) -> int {
    struct ShellError {
        DWORD win_error;
        DWORD exit_code;
    };
    const auto try_shell = [username](
        const std::filesystem::path &shell,
        std::wstring command = {}) -> std::expected<void, ShellError> {
        auto startup = STARTUPINFOW{.cb = sizeof(STARTUPINFOW)};
        auto process = wil::unique_process_information{};
        // With zero creation flags, `CreateProcessWithLogonW` creates a new
        // console for a console application. Windows Terminal creates its own
        // window. A null `STARTUPINFO::lpDesktop` makes the child inherit the
        // supervisor's window station and desktop. See
        // <https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createprocesswithlogonw>.
        if (!CreateProcessWithLogonW(
                username.c_str(), L".",
                ResetBackupAccountPassword(username).c_str(),
                LOGON_WITH_PROFILE, shell.c_str(),
                command.empty() ? nullptr : command.data(), 0,
                nullptr, nullptr, &startup, &process)) {
            return std::unexpected{ShellError{.win_error = GetLastError()}};
        }
        constexpr auto kProcessStartWait = std::chrono::milliseconds{300};
        std::this_thread::sleep_for(kProcessStartWait);
        auto exit_code = DWORD{};
        if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
            return std::unexpected{ShellError{.win_error = GetLastError()}};
        } else if ((exit_code != STILL_ACTIVE) && (exit_code != 0)) {
            return std::unexpected{ShellError{.exit_code = exit_code}};
        }
        return {};
    };
    // Package discovery and registration can overlap. `ResetBackupAccountPassword`
    // serializes each password reset with the logon that uses it.
    auto terminal_preparation = std::async(
        std::launch::async, FindAndPrepareWindowsTerminal, username);
    auto profile_preparation = std::async(std::launch::async, [username] {
        std::ignore = TryRunConsolePreparation(username,
            "PowerShell profile preparation"sv,
            std::string{kPreparePowerShellProfileOption});
    });
    const auto powershell = FindAndPreparePowerShell(username);
    profile_preparation.get();
    if (powershell) {
        if (const auto terminal = terminal_preparation.get()) {
            // Supplying the PowerShell executable explicitly keeps Terminal's
            // profile selection from substituting another shell. The new-window
            // option also overrides its preference for reusing a window.
            // Terminal splits commands at semicolons even within quoted
            // arguments, so escape any semicolons in a custom PowerShell path
            // before applying ordinary Windows command-line quoting. See
            // <https://github.com/microsoft/terminal/blob/main/src/cascadia/TerminalApp/AppCommandlineArgs.cpp>.
            const auto terminal_shell = powershell->string() |
                std::views::split(';') | std::views::join_with("\\;"sv) |
                std::ranges::to<std::string>();
            const auto arguments = std::array{
                terminal->string(), "-w"s, "new"s, "new-tab"s, "--"s, terminal_shell,
            };
            const auto command =
                std::filesystem::path{wil::ArgvToCommandLine(arguments)}.wstring();
            const auto status = try_shell(*terminal, command);
            if (status) {
                return 0;
            }
            devicefs::WriteToStream(devicefs::stderr,
                L"backup-supervisor: Windows Terminal for user '{}' could not "
                L"be started with this command line:\n{}\n(Windows error "
                L"0x{:08x}, exit code 0x{:08x})\nTrying Windows Console Host "
                L"instead.\n",
                std::wstring_view{username}, std::wstring_view{command},
                status.error().win_error, status.error().exit_code);
        }
        if (try_shell(*powershell)) {
            return 0;
        }
    }
    const auto shell = [] {
        auto system_directory = std::wstring{};
        if (const auto error = wil::GetSystemDirectoryW(system_directory);
            FAILED(error)) {
            WinError("could not identify the Windows system directory",
                ExplicitWin32Error::FromHresult(error));
        }
        return std::filesystem::path{system_directory} / L"cmd.exe";
    }();
    if (const auto status = try_shell(shell); !status) {
        const auto error = status.error();
        if (error.exit_code != 0) {
            devicefs::WriteToStream(devicefs::stderr,
                L"Error: backup console '{}' for user '{}' unexpectedly "
                L"closed quickly with exit code: 0x{:08x}\n",
                shell.native(), std::wstring_view{username}, error.exit_code);
            if (!wil::TryGetEnvironmentVariableW<std::wstring>(L"SSH_CONNECTION").empty()) {
                devicefs::WriteToStream(devicefs::stderr,
                    "Information: The `--backup-console` feature might not "
                    "be able to launch a console in an SSH session.\nTry using "
                    "a normal interactive Windows desktop session.\n");
            }
            return error.exit_code;
        }
        WinError("could not start console '{}' for backup user '{}'",
            std::wstring_view{shell.native()}, std::wstring_view{username},
            ExplicitWin32Error{error.win_error});
    }
    return 0;
}
