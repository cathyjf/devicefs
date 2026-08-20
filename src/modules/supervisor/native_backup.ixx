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

#include <wil/win32_helpers.h>

#include <cstdio>

export module devicefs.supervisor.native_backup;

import std;
import <wil/resource.h>;
import :internal;
import :devicefs_process;
import :manifest;
import :pbs;
import devicefs.common;
import devicefs.supervisor.configuration;
import devicefs.supervisor.embedded_artifacts;
import devicefs.supervisor.installation;
import devicefs.supervisor.vshadow;

export constexpr auto kCancelledExitCode = 130;

namespace internal {

using namespace std::chrono_literals;

[[nodiscard]] auto RunWslBackup(
    const HANDLE cancellation_event,
    const std::u8string_view snapshot_manifest,
    const std::optional<std::u8string> &namespace_override) {
    constexpr auto kPollInterval = 100ms;
    constexpr auto kTermTimeout = 45s;
    constexpr auto kKillTimeout = 30s;
    const auto control_path = std::format(
        L"/tmp/devicefs-{}", UniqueName());
    const auto pid_file = std::format(L"{}.pid", control_path);
    const auto stop_file = std::format(L"{}.stop", control_path);
    const auto computer_name =
        wil::GetEnvironmentVariableW<std::wstring>(L"COMPUTERNAME");
    auto backup = [&] {
        const auto persistent = ResolvePersistentPaths();
        const auto configuration =
            ReadBackupConfiguration(persistent.configuration);
        auto arguments = std::vector<std::wstring_view>{
            pid_file, stop_file, computer_name};
        if (configuration.pbs_parallelize_image_upload) {
            arguments.push_back(L"--parallel-images");
        }
        auto input = SecureUtf8String{};
        // start-pbs.fish consumes these NUL-delimited records in order. Add
        // future records here and matching Fish reads before the remaining key
        // document, which proxmox-backup-client reads through fd 0.
        const auto append_record = [&](const std::u8string_view value) {
            input.append(value);
            input.push_back(u8'\0');
        };
        append_record(configuration.wsl_client_path);
        append_record(configuration.pbs_server);
        const auto port = std::to_string(configuration.pbs_port);
        append_record(std::u8string{port.begin(), port.end()});
        append_record(configuration.pbs_datastore);
        append_record(configuration.pbs_auth_id);
        append_record(namespace_override
            ? *namespace_override : configuration.pbs_namespace);
        append_record(configuration.pbs_fingerprint);
        append_record(configuration.pbs_authentication_secret);
        append_record(snapshot_manifest);
        input.append(configuration.pbs_encryption_key);
        return StartWslFish(
            configuration,
            arguments,
            StartPbsProgram(),
            std::span<const char8_t>{input.data(), input.size()});
    }();

    while (true) {
        if (WaitForProcess(
                backup.process.hProcess, kPollInterval)) {
            return FinishWsl(backup);
        }
        const auto cancelled = WaitForSingleObject(cancellation_event, 0);
        if (cancelled == WAIT_FAILED) {
            WinError("could not inspect the backup cancellation event");
        }
        if (cancelled == WAIT_OBJECT_0) {
            break;
        }
    }

    TrySendWslBackupSignal(pid_file, stop_file, WslBackupSignal::Term);
    if (!WaitForProcess(
            backup.process.hProcess, kTermTimeout)) {
        TrySendWslBackupSignal(pid_file, stop_file, WslBackupSignal::Kill);
        if (!WaitForProcess(
                backup.process.hProcess, kKillTimeout)) {
            throw std::runtime_error(
                "the WSL backup did not exit after the KILL request");
        }
    }
    const auto exit_code = FinishWsl(backup);
    if (exit_code != 0) {
        std::wcout << L"The WSL backup exited with code " << exit_code
                   << L" during cancellation.\n";
    }
    return kCancelledExitCode;
}

[[nodiscard]] auto RunSnapshotBackup(
    const HANDLE cancellation_event,
    const devicefs::vshadow::SnapshotSet &snapshot_set,
    const std::wstring_view read_user,
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
        result = RunWslBackup(
            cancellation_event, snapshot_manifest, namespace_override);
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
    const std::span<const std::wstring> volume_override,
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
    try {
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
                    std::fwprintf(stderr, L"backup-supervisor: %hs\n",
                        error.what());
                    return kCallbackFailureExitCode;
                } catch (const std::runtime_error &error) {
                    std::fwprintf(stderr, L"backup-supervisor: %hs\n",
                        error.what());
                    return kCallbackFailureExitCode;
                }
            });
    } catch (const std::system_error &error) {
        const auto cancellation_signaled =
            WaitForSingleObject(cancellation_event, 0);
        if (cancellation_signaled == WAIT_FAILED) {
            WinError("could not inspect the backup cancellation event");
        }
        if ((cancellation_signaled == WAIT_OBJECT_0) &&
            (error.code() == std::error_code(
                ERROR_CANCELLED, std::system_category()))) {
            return kCancelledExitCode;
        }
        if (error.code() == std::error_code(
                ERROR_CANCELLED, std::system_category())) {
            throw devicefs::vshadow::OperationError(error.what());
        }
        throw;
    }
}
