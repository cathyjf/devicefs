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
import :devicefs_process;
export import :incremental_diagnostics;
export import :manifest;
import :pbs;
import devicefs.common;
import devicefs.stream_writer;
import devicefs.supervisor.configuration;
import devicefs.supervisor.launch_powershell;
import devicefs.supervisor.installation;
import devicefs.supervisor.vshadow;
import devicefs.terminal.transcoding;

using devicefs::terminal::Transcode;

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

    // WinFsp creates and removes the mount directory itself. An empty directory
    // left behind by an interrupted backup would prevent the next mount, so
    // remove it before starting the new DeviceFs child.
    const auto mount_target = std::filesystem::path{DeviceFsProcess::kMountTarget};
    if (const auto status = std::filesystem::status(mount_target);
        std::filesystem::exists(status)) {
        if (!std::filesystem::is_directory(status) ||
            !std::filesystem::is_empty(mount_target)) {
            throw std::runtime_error(std::format(
                "cannot start the backup because its mount path '{}' already "
                "exists and is not an empty directory.\n"
                "WinFsp needs this path to be absent so it can create the "
                "DeviceFs mount point. The supervisor removes empty leftover "
                "directories, but will not delete existing contents to make "
                "room for a backup.\n"
                "This path may still expose a DeviceFs filesystem from an "
                "earlier backup, or it may contain files placed there manually. "
                "Inspect the path before retrying. If an earlier DeviceFs "
                "process is still serving it, stop that process. Otherwise, "
                "inspect and move the existing file or directory aside. "
                "Then retry the backup with this path absent or empty.",
                Transcode<std::string>(mount_target.native())));
        }
        std::filesystem::remove(mount_target);
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
    return LaunchPowerShell(Transcode<std::wstring>(ReadBackupConfiguration(
        ResolvePersistentPaths().configuration).windows_username));
}
