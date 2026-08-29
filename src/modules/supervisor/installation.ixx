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

#include <devicefs/strsafe_compat.h>

#include <windows.h>
#include <sddl.h>
#include <shlobj.h>

export module devicefs.supervisor.installation;

import std;
import <devicefs/windows_imports.h>;
import devicefs.common;
import devicefs.stream_writer;
import devicefs.supervisor.configuration;

export enum class InstallMode {
    CreateOnly,
    CreateOrUpdate,
};

export struct PersistentPaths {
    std::filesystem::path root;
    std::filesystem::path logs;
    std::filesystem::path credentials;
    std::filesystem::path configuration;
    std::filesystem::path backup_lock;
};

export constexpr auto kServiceName =
    std::string_view("DeviceFsBackup");
export constexpr auto kRunServiceOption =
    std::string_view("--run-service");

namespace {

constexpr auto kProductDirectoryName = std::string_view("devicefs");
constexpr auto kExecutableName = std::string_view("backup-supervisor.exe");
constexpr auto kLogDirectoryName = std::string_view("logs");
constexpr auto kCredentialsDirectoryName = std::string_view("credentials");
constexpr auto kConfigurationName = std::string_view("backup.json");
constexpr auto kBackupLockName =
    std::string_view("pbs-vss-backup.lock");
constexpr auto kServiceDisplayName = std::string_view("DeviceFs Backup");
constexpr auto kLocalSystemAccount = std::string_view(".\\LocalSystem");
constexpr auto kNoDependencies = std::array{'\0', '\0'};

// Newly created public directories are readable and traversable by ordinary
// users, but only LocalSystem and the built-in Administrators group may modify
// them. Protecting the DACL prevents permissive parent ACEs from being inherited.
constexpr auto kPublicDirectorySecurity = wil::zstring_view(
    "O:BA"                          // Owner: built-in Administrators.
    "D:P"                           // Protected DACL.
    "(A;OICI;FA;;;SY)"              // LocalSystem: full control.
    "(A;OICI;FA;;;BA)"              // Administrators: full control.
    "(A;OICI;GRGX;;;BU)");          // Users: read and execute.
constexpr auto kExecutableSecurity = wil::zstring_view(
    "O:BA"
    "D:P"
    "(A;;FA;;;SY)"
    "(A;;FA;;;BA)"
    "(A;;GRGX;;;BU)");

// Newly created credentials objects are not readable by ordinary users.
constexpr auto kPrivateDirectorySecurity = wil::zstring_view(
    "O:BA"
    "D:P"
    "(A;OICI;FA;;;SY)"
    "(A;OICI;FA;;;BA)");
constexpr auto kPrivateFileSecurity = wil::zstring_view(
    "O:BA"
    "D:P"
    "(A;;FA;;;SY)"
    "(A;;FA;;;BA)");

[[nodiscard]] auto KnownFolderPath(
    const KNOWNFOLDERID &identifier,
    const std::string_view description) {
    // Microsoft requires COM to be initialized on the calling thread before
    // SHGetKnownFolderPath. ServiceMain is dispatched on a different thread
    // from main, so the initialization belongs at this narrow call boundary.
    const auto com_error = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const auto uninitialize =
        wil::unique_couninitialize_call(SUCCEEDED(com_error));
    if ((FAILED(com_error)) && (com_error != RPC_E_CHANGED_MODE)) {
        WinError("could not initialize COM before resolving the {} path",
            description, ExplicitWin32Error::FromHresult(com_error));
    }
    auto result = wil::unique_cotaskmem_string{};
    const auto error = SHGetKnownFolderPath(
        identifier, KF_FLAG_DEFAULT, nullptr, result.addressof());
    if (FAILED(error)) {
        WinError("could not obtain the {} path", description,
            ExplicitWin32Error::FromHresult(error));
    }
    return std::filesystem::path(result.get());
}

class SecurityDescriptor final {
public:
    explicit SecurityDescriptor(const wil::zstring_view text) {
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
                text.c_str(), SDDL_REVISION_1,
                descriptor_.addressof(), nullptr)) {
            WinError("could not create an installation security descriptor");
        }
    }

    [[nodiscard]] auto Attributes() const noexcept {
        return SECURITY_ATTRIBUTES{
            .nLength = sizeof(SECURITY_ATTRIBUTES),
            .lpSecurityDescriptor = descriptor_.get(),
            .bInheritHandle = FALSE,
        };
    }

private:
    wil::unique_hlocal_security_descriptor descriptor_;
};

auto EnsureDirectory(
    const std::filesystem::path &path,
    const SecurityDescriptor &security,
    const std::string_view description) {
    auto attributes = security.Attributes();
    if (CreateDirectoryW(path.c_str(), &attributes)) {
        return;
    }
    const auto error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) {
        return;
    }
    WinError("could not create the {}", description,
        ExplicitWin32Error{error});
}

[[nodiscard]] auto OpenBackupLock(
    const std::filesystem::path &path,
    const SecurityDescriptor &security) {
    auto attributes = security.Attributes();
    auto file = wil::unique_hfile(CreateFileW(
        path.c_str(), GENERIC_READ, 0,
        &attributes, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        const auto error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION) {
            throw std::runtime_error("a backup is already running");
        }
        WinError("could not open or create the backup lock file",
            ExplicitWin32Error{error});
    }
    return file;
}

auto CreateConfigurationTemplate(
    const std::filesystem::path &path,
    const SecurityDescriptor &security) {
    auto attributes = security.Attributes();
    auto file = wil::unique_hfile(CreateFileW(
        path.c_str(), GENERIC_WRITE, 0,
        &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_EXISTS) {
            return;
        }
        WinError("could not create the backup configuration",
            ExplicitWin32Error{error});
    }
    auto remove_incomplete_file = wil::scope_exit([&] {
        file.reset();
        static_cast<void>(DeleteFileW(path.c_str()));
    });
    constexpr auto configuration_template = GenerateConfigurationTemplate();
    [[gsl::suppress("type.4",
        justification: "Braced initialization proves this construction safe at compile time.")]]
    constexpr auto size = DWORD{configuration_template.size()};
    auto written = DWORD{};
    if (!WriteFile(file.get(), configuration_template.data(), size,
            &written, nullptr)) {
        WinError("could not write the backup configuration template");
    }
    if (written != size) {
        WinError("could not write the complete backup configuration template",
            ExplicitWin32Error{ERROR_WRITE_FAULT});
    }
    remove_incomplete_file.release();
}

auto InstallExecutable(
    const std::filesystem::path &source,
    const std::filesystem::path &destination,
    const SecurityDescriptor &security) {
    const auto destination_exists = std::filesystem::exists(destination);
    if (destination_exists &&
        (std::filesystem::equivalent(source, destination))) {
        return;
    }

    if (!destination_exists) {
        auto attributes = security.Attributes();
        auto destination_file = wil::unique_hfile(CreateFileA(
            destination.string().c_str(), 0, 0, &attributes,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!destination_file) {
            WinError("could not create the installed backup supervisor");
        }
    }
    if (!CopyFileA(source.string().c_str(), destination.string().c_str(), FALSE)) {
        WinError("could not copy the backup supervisor into Program Files");
    }
}

auto ConfigurePreshutdownTimeout(
    const SC_HANDLE service,
    const std::chrono::milliseconds preshutdown_timeout) {
    auto configuration = SERVICE_PRESHUTDOWN_INFO{
        .dwPreshutdownTimeout =
            wil::safe_cast<DWORD>(preshutdown_timeout.count()),
    };
    if (!ChangeServiceConfig2W(service,
            SERVICE_CONFIG_PRESHUTDOWN_INFO, &configuration)) {
        WinError("could not configure the backup service preshutdown timeout");
    }
}

} // namespace

export [[nodiscard]] auto CurrentExecutablePath() {
    auto result = std::wstring{};
    const auto error = wil::GetModuleFileNameW(nullptr, result);
    if (FAILED(error)) {
        WinError("could not obtain the backup supervisor path",
            ExplicitWin32Error::FromHresult(error));
    }
    return std::filesystem::path(std::move(result));
}

export [[nodiscard]] auto ProgramFilesDirectory() {
    return KnownFolderPath(FOLDERID_ProgramFiles, "Program Files");
}

export [[nodiscard]] auto ResolvePersistentPaths() {
    auto result = PersistentPaths{};
    result.root = KnownFolderPath(FOLDERID_ProgramData, "ProgramData") /
        kProductDirectoryName;
    result.logs = result.root / kLogDirectoryName;
    result.credentials = result.root / kCredentialsDirectoryName;
    result.configuration = result.credentials / kConfigurationName;
    result.backup_lock = result.credentials / kBackupLockName;
    return result;
}

export auto InstallService(
    const InstallMode mode,
    const std::chrono::milliseconds preshutdown_timeout) {
    auto manager = wil::unique_schandle(OpenSCManagerW(
        nullptr, nullptr,
        SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!manager) {
        WinError("could not open the Service Control Manager");
    }
    auto service = wil::unique_schandle(OpenServiceA(
        manager.get(), kServiceName.data(), SERVICE_CHANGE_CONFIG));
    if (service && (mode == InstallMode::CreateOnly)) {
        WinError("the backup service is already installed",
            ExplicitWin32Error{ERROR_SERVICE_EXISTS});
    }
    if (!service) {
        const auto error = GetLastError();
        if (error != ERROR_SERVICE_DOES_NOT_EXIST) {
            WinError("could not open the backup service",
                ExplicitWin32Error{error});
        }
    }

    const auto installation_directory =
        ProgramFilesDirectory() / kProductDirectoryName;
    const auto installed_executable =
        installation_directory / kExecutableName;
    const auto persistent = ResolvePersistentPaths();

    const auto public_directory =
        SecurityDescriptor(kPublicDirectorySecurity);
    const auto executable_security =
        SecurityDescriptor(kExecutableSecurity);
    const auto private_directory =
        SecurityDescriptor(kPrivateDirectorySecurity);
    const auto private_file = SecurityDescriptor(kPrivateFileSecurity);

    EnsureDirectory(
        persistent.root, public_directory,
        "devicefs data directory");
    EnsureDirectory(
        persistent.credentials, private_directory,
        "credentials directory");

    // The backup orchestrator holds the same file without sharing for the full
    // backup. This open therefore fails instead of stopping an active backup,
    // and prevents another backup from starting while installation is active.
    auto backup_lock = OpenBackupLock(
        persistent.backup_lock, private_file);

    CreateConfigurationTemplate(
        persistent.configuration, private_file);

    EnsureDirectory(
        persistent.logs, public_directory,
        "log directory");
    EnsureDirectory(
        installation_directory, public_directory,
        "devicefs installation directory");

    InstallExecutable(
        CurrentExecutablePath(), installed_executable, executable_security);
    const auto installed_executable_text = installed_executable.string();
    const auto binary_path = wil::ArgvToCommandLine(std::array{
        std::string_view(installed_executable_text),
        kRunServiceOption,
    });

    if (service) {
        if (!ChangeServiceConfigA(service.get(),
                SERVICE_WIN32_OWN_PROCESS,
                SERVICE_DEMAND_START,
                SERVICE_ERROR_NORMAL,
                binary_path.c_str(), "", nullptr, kNoDependencies.data(),
                kLocalSystemAccount.data(), "",
                kServiceDisplayName.data())) {
            WinError("could not update the backup service");
        }
        ConfigurePreshutdownTimeout(service.get(), preshutdown_timeout);
        devicefs::WriteToStream(
            devicefs::stdout, "backup-supervisor: updated {} at {}\n",
            kServiceName, installed_executable_text);
        return;
    }

    service.reset(CreateServiceA(
        manager.get(), kServiceName.data(), kServiceDisplayName.data(),
        SERVICE_CHANGE_CONFIG | DELETE,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
        binary_path.c_str(), nullptr, nullptr, nullptr,
        kLocalSystemAccount.data(), ""));
    if (!service) {
        WinError("could not install the backup service");
    }
    auto remove_incomplete_service = wil::scope_exit([&] {
        static_cast<void>(DeleteService(service.get()));
    });
    ConfigurePreshutdownTimeout(service.get(), preshutdown_timeout);
    remove_incomplete_service.release();
    devicefs::WriteToStream(
        devicefs::stdout, "backup-supervisor: installed {} at {}\n",
        kServiceName, installed_executable_text);
}
