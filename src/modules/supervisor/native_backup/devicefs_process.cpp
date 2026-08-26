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

#define RPC_USE_NATIVE_WCHAR

#include <devicefs/rpc_block_device.h>
#include <devicefs/strsafe_compat.h>

module devicefs.supervisor.native_backup:devicefs_process;

import std;
import <devicefs/windows_imports.h>;
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

struct DeviceFsSource {
    std::wstring name;
    std::wstring source;
};

struct DeviceFsStartRequest {
    std::span<const DeviceFsSource> sources;
    std::wstring_view mount_target;
    std::optional<std::wstring_view> read_user;
    std::optional<wil::zwstring_view> rpc_endpoint;
    bool vhdx = false;
};

[[nodiscard]] auto StartDeviceFs(
    const DeviceFsStartRequest &request) {
    const auto supervisor = CurrentExecutablePath();
    auto mount_target = std::wstring{request.mount_target};
    auto stop_event_name = std::format(
        L"Global\\devicefs-stop-{}", UniqueName());
    auto arguments = std::vector<std::wstring>{
        supervisor.native(),
        L"--devicefs",
        L"--synthetic-free-clusters",
        L"--cache",
    };
    if (request.vhdx) {
        arguments.emplace_back(L"--vhdx");
    }
    arguments.emplace_back(L"--mount");
    arguments.emplace_back(mount_target);
    if (request.read_user) {
        arguments.emplace_back(L"--read-user");
        arguments.emplace_back(*request.read_user);
    }
    arguments.emplace_back(L"--stop-event");
    arguments.emplace_back(stop_event_name);

    auto readiness_path = std::filesystem::path{};
    for (const auto &source : request.sources) {
        if (readiness_path.empty()) {
            readiness_path = std::filesystem::path(
                std::format(L"{}\\", mount_target)) /
                source.name;
        }
        arguments.emplace_back(L"--map");
        arguments.emplace_back(source.name);
        arguments.emplace_back(request.rpc_endpoint
            ? std::format(LR"(\\\{})", source.source)
            : source.source);
    }
    auto command = wil::ArgvToCommandLine(arguments);
    if (request.rpc_endpoint &&
        !SetEnvironmentVariableW(
            DEVICEFS_RPC_ENDPOINT_ENVIRONMENT_VARIABLE,
            request.rpc_endpoint->c_str())) {
        WinError("could not set the RPC block-device endpoint");
    }
    auto &output = devicefs::WriteToStream(
        std::cout, L"Setting up virtual filesystem: {}\n", command);
    output.flush();
    const auto creation_flags = EXTENDED_STARTUPINFO_PRESENT |
        (request.rpc_endpoint ? CREATE_SUSPENDED : DWORD{});
    auto process = StartProcessWithHandles(
        GetStdHandle(STD_INPUT_HANDLE),
        GetStdHandle(STD_OUTPUT_HANDLE),
        GetStdHandle(STD_ERROR_HANDLE),
        [&](STARTUPINFOW *const startup, PROCESS_INFORMATION *const result) {
            return CreateProcessW(
                supervisor.c_str(), command.data(),
                nullptr, nullptr, TRUE,
                creation_flags,
                nullptr,
                nullptr, startup, result);
        },
        "could not start devicefs");
    return DeviceFsProcess{
        .process = std::move(process),
        .readiness_path = std::move(readiness_path),
        .stop_event_name = std::move(stop_event_name),
    };
}

auto ResumeDeviceFs(const DeviceFsProcess &devicefs) {
    if (ResumeThread(devicefs.process.hThread) == MAXDWORD) {
        WinError("could not resume devicefs");
    }
}

[[nodiscard]] auto StartDeviceFs(
    const std::span<const devicefs::vshadow::Snapshot> snapshots,
    const std::wstring_view read_user) {
    const auto sources = snapshots |
        std::views::transform([](const auto &snapshot) {
            return DeviceFsSource{
                .name = SnapshotImageName(snapshot),
                .source = snapshot.device,
            };
        }) |
        std::ranges::to<std::vector<DeviceFsSource>>();
    return StartDeviceFs(DeviceFsStartRequest{
        .sources = sources,
        .mount_target = DeviceFsProcess::kMountTarget,
        .read_user = read_user,
    });
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
                "devicefs did not mount before the startup timeout elapsed");
        }
    }
}

struct DeviceFsExit {
    std::size_t process_index;
    DWORD exit_code;
};

[[nodiscard]] auto WaitForDeviceFsExitOrCancellation(
    const std::span<const DeviceFsProcess *const> devicefs_processes,
    const HANDLE cancellation_event) -> std::optional<DeviceFsExit> {
    auto handles = devicefs_processes |
        std::views::transform([](const auto devicefs) {
            return devicefs->process.hProcess;
        }) |
        std::ranges::to<std::vector<HANDLE>>();
    handles.push_back(cancellation_event);
    const auto result = WaitForMultipleObjects(
        wil::safe_cast_failfast<DWORD>(handles.size()),
        handles.data(), FALSE, INFINITE);
    if (result == WAIT_FAILED) {
        WinError("could not wait for devicefs or cancellation");
    }
    [[gsl::suppress("type.4",
        justification: "Braced initialization proves this construction safe at compile time.")]]
    const auto process_index =
        std::size_t{result - WAIT_OBJECT_0};
    if (process_index < devicefs_processes.size()) {
        return DeviceFsExit{
            .process_index = process_index,
            .exit_code = ProcessExitCode(handles[process_index]),
        };
    }
    return std::nullopt;
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

class DeviceFsChild {
  public:
    explicit DeviceFsChild(DeviceFsProcess devicefs) noexcept
        : devicefs_{std::move(devicefs)} {}

    DeviceFsChild(const DeviceFsChild &) = delete;
    auto operator=(const DeviceFsChild &)
        -> DeviceFsChild & = delete;
    DeviceFsChild(DeviceFsChild &&) = delete;
    auto operator=(DeviceFsChild &&)
        -> DeviceFsChild & = delete;

    ~DeviceFsChild() {
        if (running_) {
            TryStopDeviceFs(devicefs_);
        }
    }

    [[nodiscard]] auto Process() const noexcept
        -> const DeviceFsProcess & {
        return devicefs_;
    }

    auto Stop() {
        if (!running_) {
            return;
        }
        StopDeviceFs(devicefs_, false);
        running_ = false;
    }

  private:
    DeviceFsProcess devicefs_;
    bool running_ = true;
};

} // namespace internal
