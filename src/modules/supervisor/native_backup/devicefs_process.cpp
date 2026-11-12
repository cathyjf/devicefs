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
import <devicefs/common.h>;
import <devicefs/malloca.h>;
import <devicefs/windows_imports.h>;
import :internal;
import devicefs.bitmap_set;
import devicefs.constant_sequence;
import devicefs.file_reader;
import devicefs.guid_formatter;
import devicefs.object_creator;
import devicefs.rpc_constants;
import devicefs.stream_writer;
import devicefs.supervisor.installation;
import devicefs.supervisor.math;
import devicefs.supervisor.process_launch;
import devicefs.supervisor.temporary_paths;
import devicefs.supervisor.vshadow;
import devicefs.terminal.safecast;
import devicefs.terminal.transcoding;

using devicefs::terminal::Transcode;

namespace internal {

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using namespace wil::literals;

struct DeviceFsSource {
    std::string name;
    devicefs::ConstantString<char, 36, std::uint32_t> source;
};

struct DeviceFsStartRequest {
    std::span<const DeviceFsSource> sources;
    std::span<const HANDLE> inherited_handles;
    wil::zstring_view mount_target;
    wil::zstring_view read_user;
    std::optional<std::string> rpc_endpoint;
    std::optional<std::string_view> rpc_password;
    bool vhdx = false;
    bool expose_known_data_map = false;
};

class DeviceFsStopEventName {
    constexpr static auto kPrefix = "Global\\devicefs-stop-"sv;
    constexpr static auto kPrefixLength = kPrefix.length();
    constexpr static auto kSuffixLength = GuidLength<false>;
    constexpr static auto kTotalLength = kPrefixLength + kSuffixLength;
public:
    explicit DeviceFsStopEventName() noexcept {
        memcpy(buffer_.data(), kPrefix.data(), kPrefixLength);
        FormatGuidToBuffer<false>(GenerateRandomGuid(),
            std::span{buffer_}.subspan<kPrefixLength, kSuffixLength>());
        buffer_[kTotalLength] = '\0';
    }
    constexpr auto c_str() const noexcept {
        return buffer_.data();
    }
    constexpr operator wil::zstring_view() const noexcept {
        return {buffer_.data(), kTotalLength};
    }
private:
    std::array<char, kTotalLength + 1> buffer_;
};

struct DeviceFsProcess {
    static constexpr auto kPollInterval = 100ms;
    static constexpr auto kStartTimeout = 30s;
    static constexpr auto kShutdownTimeout = 60s;
    static constexpr auto kMountTarget =
        "C:\\ProgramData\\devicefs\\backup-mount"_zv;

    DeviceFsStopEventName stop_event_name;
    wil::unique_event_nothrow stop_event;
    std::filesystem::path readiness_path;
    wil::unique_process_information process;

    explicit DeviceFsProcess(const DeviceFsStartRequest &);

    auto ResumeProcess() {
        if (ResumeThread(process.hThread) == MAXDWORD) {
            WinError("failed to resume devicefs");
        }
    }
private:
    auto StartProcess(const DeviceFsStartRequest &);
};

[[nodiscard]] auto TemporaryDeviceFsViewPath() {
    return TemporarySystemDirectoryPath(L"devicefs-view");
}

template <std::unsigned_integral auto Number>
consteval auto ConvertNumberToString() noexcept {
    static constexpr auto kStringArray = [] static consteval noexcept {
        constexpr auto digit_count = [] static consteval noexcept {
            auto count = std::size_t{1};
            for (auto value = Number; value >= 10; value /= 10) {
                ++count;
            }
            return count;
        }();
        auto text = std::array<char, digit_count + 1>{};
        std::to_chars(text.data(), text.data() + digit_count, Number);
        return text;
    }();
    return wil::zstring_view{kStringArray.data(), kStringArray.size() - 1};
}

auto ConstructReadinessPath(const std::string_view target,
    const std::string_view source) {
    const auto length = devicefs::math::SafeAugend{1uz} +
        target.length() + source.length();
    const auto buffer = NEW_MALLOCA_BUFFER(char, length.integer);
    const auto span = std::span{buffer.get(), length.integer};
    devicefs::JoinSequences(span.data(), target, '\\', source);
    return std::filesystem::path{Transcode<std::wstring>(span)};
}

auto CreateRpcPasswordChannelAndWritePassword(const std::string_view password) {
    auto password_input = wil::unique_handle{};
    auto password_output = wil::unique_handle{};
    if (!CreatePipe(password_input.addressof(),
            password_output.addressof(), nullptr, 0)) {
        WinError("failed to create the devicefs password channel");
    }
    const auto write_password = [output = password_output.get()](
        const auto &value) {
        if (const auto result = WriteEntireBuffer(output, value); !result) {
            WinError("failed to write the devicefs RPC password",
                result.error());
        }
    };
    write_password(password);
    constexpr auto newline = std::array{'\n'};
    write_password(newline);
    return password_input;
}

auto DeviceFsProcess::StartProcess(const DeviceFsStartRequest &request) {
    const auto argument_count =
        devicefs::math::SafeAugend{8uz} +
        devicefs::math::SafeProduct(3uz, std::size(request.sources)) +
        (request.read_user.empty() ? 0uz : 2uz) +
        (request.vhdx ? 1uz :
            ((!request.rpc_endpoint && request.expose_known_data_map) ?
                2uz : 0uz));
    const auto argument_initial_buffer =
        NEW_MALLOCA_BUFFER(wil::zstring_view, argument_count.integer);
    auto arguments = devicefs::DisjointReallocVector{
        std::span{argument_initial_buffer.get(), argument_count.integer}};

    const auto executable_path =
        Transcode<std::string>(CurrentExecutablePath().native());
    arguments.emplace_back(executable_path);
    arguments.emplace_back("--devicefs"_zv);
    arguments.emplace_back("--synthetic-free-clusters"_zv);
    arguments.emplace_back("--cache"_zv);

    if (request.vhdx) {
        arguments.emplace_back("--vhdx"_zv);
    } else if (!request.rpc_endpoint && request.expose_known_data_map) {
        arguments.emplace_back("--expose-known-data-map"_zv);
        arguments.emplace_back(ConvertNumberToString<
            kPbsImageChunkSize>());
    }
    arguments.emplace_back("--mount"_zv);
    arguments.emplace_back(request.mount_target);
    if (!request.read_user.empty()) {
        arguments.emplace_back("--read-user"_zv);
        arguments.emplace_back(request.read_user);
    }
    arguments.emplace_back("--stop-event"_zv);
    arguments.emplace_back(stop_event_name);

    const auto source_string_count =
        request.rpc_endpoint ? std::size(request.sources) : 0;
    const auto source_string_buffer = NEW_MALLOCA_BUFFER(
        std::string, source_string_count);
    const auto source_strings = source_string_buffer.CreateObjects(
        request.sources, [&request, &arguments]
        (auto *const buffer, const auto &source) {
        arguments.emplace_back("--map"_zv);
        arguments.emplace_back(source.name);
        if (!request.rpc_endpoint) {
            arguments.emplace_back(source.source);
            return devicefs::ObjectCreationDisposition::SkipThisObject;
        }

        auto &argument = arguments.emplace_back();
        const auto &string = *::new (buffer) std::string{
            std::format("{}{}", devicefs::rpc::kDeviceSourcePrefix,
                source.source)};
        argument = string;
        return devicefs::ObjectCreationDisposition::ObjectCreated;
    });

    if (request.rpc_endpoint &&
        !SetEnvironmentVariableA(
            devicefs::rpc::kEndpointEnvironmentVariable.c_str(),
            request.rpc_endpoint->c_str())) {
        WinError("failed to set environment variable '{}' to RPC "
            "endpoint '{}'", devicefs::rpc::kEndpointEnvironmentVariable,
            *request.rpc_endpoint);
    }

    return StartProcessWithHandles(ProcessLaunchOptions{
        .argv = arguments,
        .diagnostic = [](const auto &command_line) static noexcept {
            devicefs::WriteToStream(
                devicefs::stdout, "Setting up virtual filesystem: {}\n"sv,
                command_line);
        },
        .flags = request.rpc_endpoint ? CREATE_SUSPENDED : DWORD{},
        .standard_input = request.rpc_password ?
            CreateRpcPasswordChannelAndWritePassword(
                *request.rpc_password).get() :
            GetStdHandle(STD_INPUT_HANDLE),
        .inherited_handles = request.inherited_handles,
    });
}

// Precondition: `request.sources` is nonempty.
DeviceFsProcess::DeviceFsProcess(const DeviceFsStartRequest &request) :
    stop_event{[this] {
        auto stop_event = wil::unique_event_nothrow{CreateEventExA(
            nullptr, stop_event_name.c_str(),
            CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS)};
        if (!stop_event) {
            WinError("failed to create devicefs stop event '{}'",
                wil::zstring_view{stop_event_name});
        }
        return stop_event;
    }()},
    readiness_path{ConstructReadinessPath(
        request.mount_target, request.sources.front().name)},
    process{StartProcess(request)} {}

[[nodiscard]] auto StartDeviceFs(
    const std::span<const devicefs::vshadow::Snapshot> snapshots,
    const wil::zstring_view read_user,
    const bool use_known_data_map,
    const SnapshotHandles &snapshot_handles,
    const std::span<const NamedFile> dirty_maps) {
    const auto handle_count = devicefs::math::SafeSum(
        std::size(snapshot_handles), std::size(dirty_maps));
    const auto inherited_handles = NEW_MALLOCA_BUFFER(HANDLE, handle_count);
    auto handle_index = 0uz;
    const auto add_handle = [&handle_index, &inherited_handles]
        (const auto &handle) noexcept {
        inherited_handles[handle_index++] = handle.get();
    };
    for (const auto &handle : snapshot_handles | std::views::values) {
        add_handle(handle);
    }

    const auto source_count = devicefs::math::SafeSum(
        std::size(snapshots), std::size(dirty_maps));
    const auto source_buffer = NEW_MALLOCA_BUFFER(DeviceFsSource, source_count);
    const auto sources_first = source_buffer.CreateObjects(snapshots,
        [&snapshot_handles](const auto &snapshot) {
        return DeviceFsSource{
            .name = SnapshotImageName(snapshot),
            .source{devicefs::rpc::kHandleDevicePrefix,
                std::to_string(std::bit_cast<std::uintptr_t>(
                    snapshot_handles.at(snapshot.identifier).get()))}};
    });
    const auto sources_second = devicefs::CreateObjects(
        sources_first.data() + sources_first.size(), dirty_maps,
        [&add_handle](const auto &entry) {
        const auto &[name, handle] = entry;
        add_handle(handle);
        return DeviceFsSource{
            .name = name,
            .source{devicefs::rpc::kFileHandleDevicePrefix,
                std::to_string(std::bit_cast<std::uintptr_t>(handle.get()))}};
    });

    return DeviceFsProcess{DeviceFsStartRequest{
        .sources = std::span{source_buffer.get(), source_count},
        .inherited_handles = std::span{inherited_handles.get(), handle_count},
        .mount_target = DeviceFsProcess::kMountTarget,
        .read_user = read_user,
        .expose_known_data_map = use_known_data_map,
    }};
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
                Transcode<std::string>(devicefs.readiness_path.native())));
        }
        if (wil::event_is_signaled(cancellation_event)) {
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
                Transcode<std::string>(devicefs.readiness_path.native()),
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
    const HANDLE cancellation_event,
    const auto &...devicefs_processes) -> std::optional<DeviceFsExit> {
    const auto result = wil::wait_any_failfast(
        devicefs_processes.process.hProcess..., cancellation_event);
    const auto process_index =
        CompileTimeCast<std::size_t>(result.status() - WAIT_OBJECT_0);
    if (process_index < sizeof...(devicefs_processes)) {
        const auto processes = std::array{&devicefs_processes...};
        return DeviceFsExit{
            .process_index = process_index,
            .exit_code = ProcessExitCode(
                processes[process_index]->process.hProcess),
        };
    }
    return std::nullopt;
}

class DeviceFsChild {
public:
    explicit DeviceFsChild(DeviceFsProcess devicefs,
        std::optional<std::reference_wrapper<bool>> exit_confirmed =
            std::nullopt) noexcept
        : devicefs_{std::move(devicefs)}, exit_confirmed_{exit_confirmed} {
        if (exit_confirmed_) {
            exit_confirmed_->get() = false;
        }
    }

    DeviceFsChild(const DeviceFsChild &) = delete;
    auto operator=(const DeviceFsChild &)
        -> DeviceFsChild & = delete;
    DeviceFsChild(DeviceFsChild &&) = delete;
    auto operator=(DeviceFsChild &&)
        -> DeviceFsChild & = delete;

    ~DeviceFsChild() {
        // Windows must still be able to read a VHDX's backing file to detach
        // the disk. If detachment failed, terminating DeviceFs could make the
        // disk impossible to detach without rebooting. To mitigate that, we
        // signal an event to request that DeviceFs shut down gracefully. After
        // receipt of that signal, DeviceFs waits for open files to close before
        // exiting, so that it can continue serving a VHDX that remains attached.
        devicefs_.stop_event.SetEvent();
        if (WaitForProcess(devicefs_.process.hProcess,
                DeviceFsProcess::kShutdownTimeout)) {
            if (exit_confirmed_) {
                exit_confirmed_->get() = true;
            }
            if (auto exit_code = DWORD{};
                GetExitCodeProcess(devicefs_.process.hProcess, &exit_code) &&
                    (exit_code != ERROR_SUCCESS)) {
                devicefs::WriteToStream(devicefs::stderr,
                    "backup-supervisor: informational: {}\n",
                    TryConstructWinError("devicefs child process {} exited "
                        "with a nonzero status",
                        CompileTimeCast<std::uint32_t>(
                            devicefs_.process.dwProcessId),
                        ExplicitWin32Error{exit_code}));
            }
            return;
        }
        devicefs::WriteToStream(devicefs::stderr,
            "backup-supervisor: {}\n",
            TryConstructWinError("failed to stop devicefs child process {} "
                "(shutdown event '{}')",
                CompileTimeCast<std::uint32_t>(
                    devicefs_.process.dwProcessId),
                wil::zstring_view{devicefs_.stop_event_name},
                ExplicitWin32Error{ERROR_TIMEOUT}));
    }

    [[nodiscard]] auto Process() const noexcept
        -> const DeviceFsProcess & {
        return devicefs_;
    }

private:
    DeviceFsProcess devicefs_;
    std::optional<std::reference_wrapper<bool>> exit_confirmed_;
};

} // namespace internal
