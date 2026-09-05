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

module devicefs.supervisor.native_backup:devicefs_process;

import std;
import <devicefs/windows_imports.h>;
import :internal;
import devicefs.common;
import devicefs.rpc_constants;
import devicefs.stream_writer;
import devicefs.supervisor.installation;
import devicefs.supervisor.process_launch;
import devicefs.supervisor.temporary_paths;
import devicefs.supervisor.vshadow;

namespace internal {

using namespace std::chrono_literals;

struct DeviceFsProcess {
    static constexpr auto kPollInterval = 100ms;
    static constexpr auto kStartTimeout = 30s;
    static constexpr auto kShutdownTimeout = 60s;
    static constexpr auto kMountTarget = std::string_view("X:");
    static constexpr auto kMountDriveMask =
        DWORD{1} << (kMountTarget.front() - 'A');

    wil::unique_process_information process;
    std::filesystem::path readiness_path;
    std::string stop_event_name;
};

struct DeviceFsSource {
    std::string name;
    std::string source;
};

struct DeviceFsStartRequest {
    std::span<const DeviceFsSource> sources;
    std::string mount_target;
    std::optional<std::string> read_user;
    std::optional<std::string> rpc_endpoint;
    std::optional<std::string_view> rpc_password;
    bool vhdx = false;
};

[[nodiscard]] auto TemporaryDeviceFsViewPath() {
    return TemporarySystemDirectoryPath("devicefs-view");
}

[[nodiscard]] auto StartDeviceFs(
    const DeviceFsStartRequest &request) {
    const auto supervisor = CurrentExecutablePath().string();
    auto stop_event_name = std::format(
        "Global\\devicefs-stop-{}", UniqueName());
    auto arguments = std::vector<std::string>{
        supervisor,
        "--devicefs",
        "--synthetic-free-clusters",
        "--cache",
    };
    if (request.vhdx) {
        arguments.emplace_back("--vhdx");
    }
    arguments.emplace_back("--mount");
    arguments.emplace_back(request.mount_target);
    if (request.read_user) {
        arguments.emplace_back("--read-user");
        arguments.emplace_back(*request.read_user);
    }
    arguments.emplace_back("--stop-event");
    arguments.emplace_back(stop_event_name);

    auto readiness_path = std::filesystem::path{};
    for (const auto &source : request.sources) {
        if (readiness_path.empty()) {
            readiness_path = std::filesystem::path(
                std::format("{}\\", request.mount_target)) /
                source.name;
        }
        arguments.emplace_back("--map");
        arguments.emplace_back(source.name);
        arguments.emplace_back(request.rpc_endpoint
            ? std::format(R"(\\\{})", source.source)
            : source.source);
    }
    auto command = wil::ArgvToCommandLine(arguments);
    if (request.rpc_endpoint &&
        !SetEnvironmentVariableA(
            devicefs::rpc::kEndpointEnvironmentVariable.data(),
            request.rpc_endpoint->c_str())) {
        WinError("could not set environment variable '{}' to RPC endpoint '{}'",
            devicefs::rpc::kEndpointEnvironmentVariable,
            *request.rpc_endpoint);
    }
    devicefs::WriteToStream(
        devicefs::stdout, "Setting up virtual filesystem: {}\n", command);
    const auto creation_flags = EXTENDED_STARTUPINFO_PRESENT |
        (request.rpc_endpoint ? CREATE_SUSPENDED : DWORD{});
    auto password_input = wil::unique_handle{};
    auto password_output = wil::unique_handle{};
    if (request.rpc_password) {
        if (!CreatePipe(password_input.addressof(), password_output.addressof(),
                nullptr, 0)) {
            WinError("could not create the devicefs password channel");
        }
        const auto write_password = [&](const std::span<const char> value) {
            if (value.empty()) {
                return;
            }
            const auto size = wil::safe_cast<DWORD>(value.size_bytes());
            auto written = DWORD{};
            if (!WriteFile(password_output.get(), value.data(), size,
                    &written, nullptr)) {
                WinError("could not write the devicefs RPC password");
            }
            if (written != size) {
                WinError("could not write the complete devicefs RPC password",
                    ExplicitWin32Error{ERROR_WRITE_FAULT});
            }
        };
        write_password(std::span{
            request.rpc_password->data(), request.rpc_password->size()});
        constexpr auto newline = std::array{'\n'};
        write_password(std::span{newline});
        password_output.reset();
    }
    auto process = StartProcessWithHandles(
        password_input
            ? password_input.get() : GetStdHandle(STD_INPUT_HANDLE),
        GetStdHandle(STD_OUTPUT_HANDLE),
        GetStdHandle(STD_ERROR_HANDLE),
        [&](STARTUPINFOA *const startup, PROCESS_INFORMATION *const result) {
            return CreateProcessA(
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
    const std::string_view read_user) {
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
        .mount_target = std::string{DeviceFsProcess::kMountTarget},
        .read_user = std::string{read_user},
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
                "devicefs exited with code {} before creating readiness path '{}'",
                ProcessExitCode(devicefs.process.hProcess),
                devicefs.readiness_path.string()));
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
            throw std::runtime_error(std::format(
                "devicefs did not create readiness path '{}' within {} seconds",
                devicefs.readiness_path.string(),
                std::chrono::duration_cast<std::chrono::seconds>(
                    DeviceFsProcess::kStartTimeout).count()));
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
            auto stop_event = wil::unique_event_nothrow{OpenEventA(
                EVENT_MODIFY_STATE, FALSE,
                devicefs.stop_event_name.c_str())};
            if (stop_event) {
                if (!SetEvent(stop_event.get())) {
                    WinError("could not signal devicefs shutdown event '{}'",
                        devicefs.stop_event_name);
                }
                stop_requested = true;
            } else {
                const auto error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND) {
                    WinError("could not open devicefs shutdown event '{}'",
                        devicefs.stop_event_name,
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
            throw std::runtime_error(std::format(
                "devicefs did not exit within {} seconds after shutdown was requested",
                std::chrono::duration_cast<std::chrono::seconds>(
                    DeviceFsProcess::kShutdownTimeout).count()));
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
        if (stop_required_) {
            static_cast<void>(TryStop());
        }
    }

    [[nodiscard]] auto Process() const noexcept
        -> const DeviceFsProcess & {
        return devicefs_;
    }

    auto Stop() {
        if (!stop_required_) {
            return;
        }
        StopDeviceFs(devicefs_, false);
        stop_required_ = false;
    }

    [[nodiscard]] auto TryStop() noexcept {
        try {
            Stop();
            return true;
        } catch (const std::exception &error) {
            TryWriteError("devicefs cleanup failed", error);
            // This method owns the single shutdown attempt. Its caller can use
            // the false result to retain resources that DeviceFs may still use.
            stop_required_ = false;
            return false;
        }
    }

  private:
    DeviceFsProcess devicefs_;
    bool stop_required_ = true;
};

} // namespace internal
