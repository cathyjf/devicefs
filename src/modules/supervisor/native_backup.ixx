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
import <devicefs/common.h>;
import devicefs.stream_writer;
import devicefs.supervisor.configuration;
import devicefs.supervisor.launch_powershell;
import devicefs.supervisor.installation;
import devicefs.supervisor.vshadow;
import devicefs.terminal.transcoding;

using devicefs::terminal::Transcode;

export constexpr auto kCancelledExitCode = internal::kCancelledExitCode;

// Read the Fish program from `path` on Windows and run it in the WSL distribution
// registered to the configured backup account. The existing PBS startup code
// runs first, so the supplied program can use the configured PBS connection
// settings and authentication credentials. PBS can read the encryption key
// from the supplied program's standard input using `--keyfd 0`.
//
// Scripts that start a child should use `supervise_pbs` to publish its PID for
// cancellation and return its exit status, as the built-in PBS operations do.
export [[nodiscard]] auto RunFishProgram(
    const HANDLE cancellation_event,
    const std::filesystem::path &path,
    const std::span<const std::string_view> arguments) -> int {
    auto file = std::ifstream{path, std::ios::binary};
    if (!file.is_open()) {
        throw std::runtime_error(std::format(
            "could not open the Fish program '{}'",
            Transcode<std::string>(path.native())));
    }
    const auto program = std::string{std::istreambuf_iterator<char>{file}, {}};
    if (file.bad()) {
        throw std::runtime_error(std::format(
            "could not read the Fish program '{}'",
            Transcode<std::string>(path.native())));
    }
    const auto fish_arguments = std::array{
        std::span<const std::string_view>{
            std::array{std::string_view{"--run-fish-program"}}},
        arguments,
    } | std::views::join | std::ranges::to<std::vector>();
    const auto result = internal::RunPbsFish(
        cancellation_event, std::nullopt,
        internal::PbsFishRequest{
            .additional_arguments = fish_arguments,
            .additional_program = program,
            .send_encryption_key = true,
        });
    return result ? result->exit_code : kCancelledExitCode;
}

export [[nodiscard]] auto InventoryVhdx(
    const HANDLE cancellation_event,
    const std::string_view device) -> int;

// Retrieve the PBS snapshot catalog as UTF-8 JSON for all groups in the
// configured namespace, or the supplied override, and its descendants. The
// object contains the starting namespace and a snapshots array; each snapshot
// includes its full namespace. Cancellation returns an empty optional.
// Retrieval failures throw, with PBS diagnostics forwarded to standard error.
export [[nodiscard]] auto RetrieveBackupCatalog(
    const HANDLE cancellation_event,
    const std::optional<std::u8string> &namespace_override)
    -> std::optional<std::u8string> {
    constexpr auto arguments = std::to_array<std::string_view>({"--list-backups"});
    auto result = internal::RunPbsFish(
        cancellation_event, namespace_override,
        internal::PbsFishRequest{
            .additional_arguments = arguments,
            .standard_output = internal::PbsStandardOutput::Capture,
        });
    if (!result) {
        return std::nullopt;
    }
    if (result->exit_code != 0) {
        throw std::runtime_error(std::format(
            "the backup catalog query exited with code {}",
            result->exit_code));
    }
    return std::move(result->standard_output);
}

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
    const bool use_known_data_map,
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

    const auto devicefs = StartDeviceFs(
        snapshot_set.snapshots, read_user, use_known_data_map);
    auto cleanup = wil::scope_exit([&] {
        TryStopDeviceFs(devicefs);
    });

    auto result = kCancelledExitCode;
    if (WaitForDeviceFs(devicefs, cancellation_event)) {
        const auto pbs_result = RunPbsFish(
            cancellation_event,
            namespace_override,
            PbsFishRequest{
                .snapshot_manifest = [&snapshot_set](const std::string_view layer) {
                    return SerializeSnapshotManifest(snapshot_set, layer);
                },
                .send_encryption_key = true,
            });
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

    auto [read_user, selected_volumes, use_known_data_map] = [] {
        const auto persistent = ResolvePersistentPaths();
        auto configuration =
            ReadBackupConfiguration(persistent.configuration);
        return std::tuple{
            std::move(configuration.windows_username),
            std::move(configuration.volumes),
            configuration.pbs_use_known_data_map,
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
                        use_known_data_map,
                        namespace_override);
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
