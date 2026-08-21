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

module devicefs.supervisor.native_backup:devicefs_process;

import std;
import <wil/resource.h>;
import <wil/stl.h>;
import :internal;
import devicefs.common;
import devicefs.stream_writer;
import devicefs.supervisor.installation;
import devicefs.supervisor.vshadow;

namespace internal {

using namespace std::chrono_literals;

struct DeviceFsProcess {
    static constexpr auto kPollInterval = 100ms;
    static constexpr auto kStartTimeout = 30s;
    static constexpr auto kShutdownTimeout = 60s;
    static constexpr auto kMountTarget = std::wstring_view(L"X:");
    static constexpr auto kMountDriveMask =
        DWORD{1} << (kMountTarget.front() - L'A');

    wil::unique_process_information process;
    std::filesystem::path readiness_path;
    std::wstring stop_event_name;
};

[[nodiscard]] auto StartDeviceFs(
    const std::span<const devicefs::vshadow::Snapshot> snapshots,
    const std::wstring_view read_user) {
    const auto supervisor = CurrentExecutablePath();
    auto stop_event_name = std::format(
        L"Global\\devicefs-stop-{}", UniqueName());
    auto arguments = std::vector<std::wstring>{
        supervisor.native(),
        L"--devicefs",
        L"--synthetic-free-clusters", L"--cache",
        L"--mount", std::wstring{DeviceFsProcess::kMountTarget},
        L"--read-user", std::wstring{read_user},
        L"--stop-event", stop_event_name,
    };
    auto readiness_path = std::filesystem::path{};
    for (const auto &snapshot : snapshots) {
        auto filename = SnapshotImageName(snapshot);
        if (readiness_path.empty()) {
            readiness_path = std::filesystem::path(
                std::format(L"{}\\", DeviceFsProcess::kMountTarget)) /
                filename;
        }
        arguments.emplace_back(L"--map");
        arguments.emplace_back(std::move(filename));
        arguments.emplace_back(snapshot.device);
    }
    auto command = wil::ArgvToCommandLine(arguments);
    devicefs::WriteToStream(
        std::cout, L"Setting up virtual filesystem: {}\n", command);
    auto process = StartProcessWithHandles(
        GetStdHandle(STD_INPUT_HANDLE),
        GetStdHandle(STD_OUTPUT_HANDLE),
        GetStdHandle(STD_ERROR_HANDLE),
        [&](STARTUPINFOW *const startup, PROCESS_INFORMATION *const result) {
            return CreateProcessW(
                supervisor.c_str(), command.data(),
                nullptr, nullptr, TRUE,
                EXTENDED_STARTUPINFO_PRESENT,
                nullptr, nullptr, startup, result);
        },
        "could not start devicefs");
    return DeviceFsProcess{
        .process = std::move(process),
        .readiness_path = std::move(readiness_path),
        .stop_event_name = std::move(stop_event_name),
    };
}

[[nodiscard]] auto WaitForDeviceFs(
    const DeviceFsProcess &devicefs,
    const HANDLE cancellation_event) {
    const auto deadline =
        std::chrono::steady_clock::now() + DeviceFsProcess::kStartTimeout;
    while (true) {
        if (WaitForProcess(devicefs.process.hProcess,
                DeviceFsProcess::kPollInterval)) {
            throw std::runtime_error(std::format(
                "devicefs exited during startup with code {}.",
                ProcessExitCode(devicefs.process.hProcess)));
        }
        const auto cancelled = WaitForSingleObject(cancellation_event, 0);
        if (cancelled == WAIT_FAILED) {
            WinError("could not inspect the backup cancellation event");
        }
        if (cancelled == WAIT_OBJECT_0) {
            return false;
        }
        auto exists_error = std::error_code{};
        if (std::filesystem::is_regular_file(
                devicefs.readiness_path, exists_error)) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(
                "devicefs did not mount X: before the startup timeout elapsed");
        }
    }
}

auto StopDeviceFs(
    const DeviceFsProcess &devicefs,
    const bool backup_succeeded) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        DeviceFsProcess::kShutdownTimeout;
    auto stop_requested = false;
    while (!WaitForProcess(devicefs.process.hProcess, 0ms)) {
        if (!stop_requested) {
            auto stop_event = wil::unique_event_nothrow{};
            if (stop_event.try_open(
                    devicefs.stop_event_name.c_str(),
                    EVENT_MODIFY_STATE)) {
                if (!SetEvent(stop_event.get())) {
                    WinError("could not request devicefs shutdown");
                }
                stop_requested = true;
            } else {
                const auto error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND) {
                    WinError("could not open the devicefs shutdown event",
                        ExplicitWin32Error{error});
                }
            }
        }
        if (WaitForProcess(
                devicefs.process.hProcess,
                DeviceFsProcess::kPollInterval)) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(
                "devicefs did not exit before the shutdown timeout elapsed");
        }
    }
    if (!backup_succeeded) {
        return;
    }
    const auto exit_code = ProcessExitCode(devicefs.process.hProcess);
    if (!stop_requested) {
        throw std::runtime_error(std::format(
            "devicefs exited before shutdown was requested with code {}.",
            exit_code));
    }
    if (exit_code != 0) {
        throw std::runtime_error(std::format(
            "devicefs exited with code {}.", exit_code));
    }
}

auto TryStopDeviceFs(const DeviceFsProcess &devicefs) noexcept {
    try {
        StopDeviceFs(devicefs, false);
    } catch (const std::exception &error) {
        TryWriteError("devicefs cleanup failed", error);
    }
}

} // namespace internal
