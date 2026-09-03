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

export module devicefs.supervisor.native_backup;

import std;
import <devicefs/windows_imports.h>;
import :internal;
import :password_reset;
import :devicefs_process;
export import :incremental_diagnostics;
export import :manifest;
import :pbs;
import devicefs.common;
import devicefs.stream_writer;
import devicefs.supervisor.configuration;
import devicefs.supervisor.find_powershell;
import devicefs.supervisor.installation;
import devicefs.supervisor.vshadow;

export constexpr auto kCancelledExitCode = internal::kCancelledExitCode;

export [[nodiscard]] auto InventoryVhdx(
    HANDLE cancellation_event,
    std::string_view device) -> int;

export [[nodiscard]] auto RunSelectiveView(
    const HANDLE cancellation_event,
    const std::string_view archive,
    const std::optional<std::string_view> snapshot_override,
    const std::optional<std::string_view> timestamp,
    const std::string_view address,
    const std::optional<std::u8string> &namespace_override) -> int;

namespace internal {

[[nodiscard]] auto RunSnapshotBackup(
    const HANDLE cancellation_event,
    const devicefs::vshadow::SnapshotSet &snapshot_set,
    const std::string_view read_user,
    const std::optional<std::u8string> &namespace_override) {
    const auto cancelled = WaitForSingleObject(cancellation_event, 0);
    if (cancelled == WAIT_FAILED) {
        WinError("could not inspect the backup cancellation event");
    }
    if (cancelled == WAIT_OBJECT_0) {
        return kCancelledExitCode;
    }

    const auto logical_drives = GetLogicalDrives();
    if (logical_drives == 0) {
        WinError("could not enumerate drive letters");
    }
    if ((logical_drives & DeviceFsProcess::kMountDriveMask) != 0) {
        throw std::runtime_error("mount target is already present: X:");
    }

    const auto snapshot_manifest = SerializeSnapshotManifest(snapshot_set);
    const auto devicefs = StartDeviceFs(snapshot_set.snapshots, read_user);
    auto cleanup = wil::scope_exit([&] {
        TryStopDeviceFs(devicefs);
    });

    auto result = kCancelledExitCode;
    if (WaitForDeviceFs(devicefs, cancellation_event)) {
        const auto pbs_result = RunPbsFish(
            cancellation_event,
            namespace_override,
            PbsFishRequest{.snapshot_manifest = snapshot_manifest});
        result = pbs_result
            ? pbs_result->exit_code : kCancelledExitCode;
    }

    cleanup.release();
    try {
        StopDeviceFs(devicefs, result == 0);
    } catch (const std::exception &error) {
        if (result == 0) {
            throw;
        }
        TryWriteError("devicefs cleanup failed", error);
    }
    return result;
}

} // namespace internal

export [[nodiscard]] auto RunNativeBackup(
    const HANDLE cancellation_event,
    const bool no_writers,
    const std::span<const std::string> volume_override,
    const std::optional<std::u8string> &namespace_override) -> int {
    const auto cancelled = WaitForSingleObject(cancellation_event, 0);
    if (cancelled == WAIT_FAILED) {
        WinError("could not inspect the backup cancellation event");
    }
    if (cancelled == WAIT_OBJECT_0) {
        return kCancelledExitCode;
    }

    auto [read_user, selected_volumes] = [] {
        const auto persistent = ResolvePersistentPaths();
        auto configuration =
            ReadBackupConfiguration(persistent.configuration);
        return std::pair{
            std::move(configuration.windows_username),
            std::move(configuration.volumes),
        };
    }();
    if (!volume_override.empty()) {
        selected_volumes.assign_range(volume_override);
    }
    constexpr auto kCallbackFailureExitCode = 2;
    return internal::RunVssOperation(cancellation_event, [&] {
        return devicefs::vshadow::Run(
            cancellation_event,
            !no_writers,
            selected_volumes,
            [&](const devicefs::vshadow::SnapshotSet &snapshot_set) {
                try {
                    return internal::RunSnapshotBackup(
                        cancellation_event,
                        snapshot_set,
                        read_user,
                        namespace_override);
                } catch (const wil::ResultException &error) {
                    devicefs::WriteToStream(
                        devicefs::stderr, "backup-supervisor: {}\n", error.what());
                    return kCallbackFailureExitCode;
                } catch (const std::runtime_error &error) {
                    devicefs::WriteToStream(
                        devicefs::stderr, "backup-supervisor: {}\n", error.what());
                    return kCallbackFailureExitCode;
                }
            });
    });
}

export [[nodiscard]] auto RunBackupConsole() -> int {
    if (internal::RunningAsLocalSystem()) {
        throw std::runtime_error(
            "--backup-console does not support invocation as LocalSystem");
    }
    const auto username = std::filesystem::path{ReadBackupConfiguration(
        ResolvePersistentPaths().configuration).windows_username}.wstring();
    struct ShellError {
        DWORD win_error;
        DWORD exit_code;
    };
    const auto try_start_shell = [&username](
        const std::filesystem::path &shell) -> std::expected<void, ShellError> {
        auto startup = STARTUPINFOW{.cb = sizeof(STARTUPINFOW)};
        auto process = wil::unique_process_information{};
        // With zero creation flags, `CreateProcessWithLogonW` creates a new
        // console. A null `STARTUPINFO::lpDesktop` makes the child inherit the
        // supervisor's window station and desktop. See
        // <https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createprocesswithlogonw>.
        if (!CreateProcessWithLogonW(
                username.c_str(), internal::kLocalDomain.c_str(),
                internal::ResetBackupAccountPassword(username).c_str(),
                LOGON_WITH_PROFILE, shell.c_str(), nullptr, 0,
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
    if (const auto powershell = PowerShellPath();
        powershell && try_start_shell(*powershell)) {
        return 0;
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
    if (const auto status = try_start_shell(shell); !status) {
        const auto error = status.error();
        if (error.exit_code != 0) {
            devicefs::WriteToStream(devicefs::stderr,
                L"Error: backup console '{}' for user '{}' unexpectedly "
                L"closed quickly with exit code: 0x{:08x}\n",
                shell.native(), username, error.exit_code);
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
