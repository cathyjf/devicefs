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
import <devicefs/malloca_list.h>;
import <devicefs/windows_imports.h>;
import :internal;
import devicefs.bitmap_set;
import devicefs.constant_sequence;
import devicefs.file_reader;
import devicefs.guid_formatter;
import devicefs.object_creator;
import devicefs.packed_view;
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
    devicefs::ConstantString<char, 62, std::uint16_t> name;
    devicefs::ConstantString<char, 36, std::uint32_t> source;
};

struct DeviceFsStartRequest {
    std::span<const DeviceFsSource> sources;
    std::span<const HANDLE> inherited_handles;
    wil::zstring_view mount_target;
    wil::zstring_view read_user;
    wil::zstring_view rpc_endpoint;
    std::optional<std::string_view> rpc_password;
    bool vhdx = false;
    bool expose_known_data_map = false;
};

namespace stop_event_detail {

auto MakeSequence() noexcept {
    using namespace devicefs::literals;
    return MakeExactSequence<char>("Global\\devicefs-stop-"_text,
        UniqueNameSequence(), '\0');
}

} // namespace stop_event_detail

class DeviceFsStopEventName {
public:
    constexpr auto c_str() const noexcept {
        return name_.data();
    }
    constexpr operator wil::zstring_view() const noexcept {
        return {name_.data(), name_.size() - 1};
    }
private:
    decltype(stop_event_detail::MakeSequence()) name_ =
        stop_event_detail::MakeSequence();
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
    auto Resume();
    [[nodiscard]] auto WaitUntilReady(const HANDLE cancellation_event) const;
    [[nodiscard]] auto WaitForExit(const HANDLE cancellation_event,
        const auto &...other_devicefs_processes) const;
};

namespace devicefs_process_detail {

template <std::unsigned_integral auto Number>
consteval auto ConvertNumberToString() noexcept {
    static constexpr auto kStringArray = [] static consteval noexcept {
        constexpr auto digit_count = devicefs::math::CountDigits(Number);
        auto text = std::array<char, digit_count + 1>{};
        std::to_chars(text.data(), text.data() + digit_count, Number);
        return text;
    }();
    return wil::zstring_view{kStringArray.data(), kStringArray.size() - 1};
}

auto ConstructReadinessPath(const std::string_view target,
    const std::string_view source) {
    const auto sequence = devicefs::DeferredSequence{target, '\\', source};
    const auto length = devicefs::SequenceLength(sequence);
    const auto buffer = NEW_MALLOCA_BUFFER(char, length);
    const auto span = std::span{buffer.get(), length};
    devicefs::JoinSequences(span.data(), sequence);
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

auto StartProcess(const DeviceFsStartRequest &request,
    const DeviceFsStopEventName &stop_event_name) {
    using PackedView = devicefs::PackedTerminatedStringView<char, 3>;

    const auto executable_path =
        Transcode<std::string>(CurrentExecutablePath().native());
    const auto range_one = std::to_array<PackedView>({
        executable_path,
        "--devicefs"_zv,
        "--synthetic-free-clusters"_zv,
        "--cache"_zv,
        "--mount"_zv, request.mount_target,
        "--stop-event"_zv, wil::zstring_view{stop_event_name}});
    const auto range_two = request.vhdx ?
        MALLOCA_LIST(PackedView, "--vhdx"_zv) :
            ((request.rpc_endpoint.empty() && request.expose_known_data_map) ?
                MALLOCA_LIST(PackedView, "--expose-known-data-map"_zv,
                    ConvertNumberToString<kPbsImageChunkSize>()) :
                MALLOCA_LIST(PackedView));
    const auto range_three = (!request.read_user.empty()) ?
        MALLOCA_LIST(PackedView, "--read-user"_zv, request.read_user) :
        MALLOCA_LIST(PackedView);

    const auto source_argument_count =
        devicefs::math::SafeProduct(3uz, std::size(request.sources));
    const auto source_argument_initial_buffer =
        NEW_MALLOCA_BUFFER(PackedView, source_argument_count);
    auto source_arguments = devicefs::DisjointReallocVector{std::span{
        source_argument_initial_buffer.get(), source_argument_count}};

    const auto rpc_source_buffer_length = request.rpc_endpoint.empty() ? 0uz :
        std::ranges::fold_left(request.sources, devicefs::math::SafeAugend{0uz},
            [](const auto length, const auto &source) static {
                return length + source.source.length() +
                    devicefs::rpc::kDeviceSourcePrefix.length() + 1;
            }).integer;
    const auto rpc_source_buffer =
        NEW_MALLOCA_BUFFER(char, rpc_source_buffer_length);
    auto rpc_source_cursor = rpc_source_buffer.get();

    for (const auto &source : request.sources) {
        source_arguments.emplace_back("--map"_zv);
        source_arguments.emplace_back(source.name);
        if (request.rpc_endpoint.empty()) {
            source_arguments.emplace_back(source.source);
            continue;
        }

        const auto *const first = rpc_source_cursor;
        rpc_source_cursor = devicefs::JoinSequences(rpc_source_cursor,
            devicefs::rpc::kDeviceSourcePrefix,
            std::string_view{source.source}, '\0');
        source_arguments.emplace_back(first, rpc_source_cursor - first - 1);
    }

    if (!request.rpc_endpoint.empty() &&
        !SetEnvironmentVariableA(
            devicefs::rpc::kEndpointEnvironmentVariable.c_str(),
            request.rpc_endpoint.c_str())) {
        WinError("failed to set environment variable '{}' to RPC "
            "endpoint '{}'", devicefs::rpc::kEndpointEnvironmentVariable,
            request.rpc_endpoint);
    }

    return StartProcessWithHandles(ProcessLaunchOptions{
        .argv = devicefs::DisjointRange{range_one, range_two} +
            range_three + source_arguments,
        .diagnostic = [](const auto &command_line) static noexcept {
            devicefs::WriteToStream(
                devicefs::stdout, "Setting up virtual filesystem: {}\n"sv,
                command_line);
        },
        .flags = request.rpc_endpoint.empty() ? DWORD{} : CREATE_SUSPENDED,
        .standard_input = request.rpc_password ?
            CreateRpcPasswordChannelAndWritePassword(
                *request.rpc_password).get() :
            GetStdHandle(STD_INPUT_HANDLE),
        .inherited_handles = request.inherited_handles,
    });
}

} // namespace devicefs_process_detail

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
    readiness_path{devicefs_process_detail::ConstructReadinessPath(
        request.mount_target, request.sources.front().name)},
    process{devicefs_process_detail::StartProcess(request, stop_event_name)} {}

auto DeviceFsProcess::Resume() {
    if (ResumeThread(process.hThread) == MAXDWORD) {
        WinError("failed to resume devicefs");
    }
}

namespace devicefs_child_detail {

[[nodiscard]] auto StartDeviceFs(
    const auto &snapshot_handles,
    const wil::zstring_view read_user,
    const bool use_known_data_map,
    const auto &dirty_maps) {
    const auto handle_count = devicefs::math::SafeSum(
        std::size(snapshot_handles), std::size(dirty_maps));
    const auto inherited_handles = NEW_MALLOCA_BUFFER(HANDLE, handle_count);
    auto handle_index = 0uz;
    const auto add_handle = [&handle_index, &inherited_handles]
        (const auto &handle) noexcept {
        inherited_handles[handle_index++] = handle;
    };

    const auto source_count = devicefs::math::SafeSum(
        std::size(snapshot_handles), std::size(dirty_maps));
    const auto source_buffer = NEW_MALLOCA_BUFFER(DeviceFsSource, source_count);
    const auto sources_first = source_buffer.CreateObjects(snapshot_handles,
        [&add_handle](const auto &entry) {
        const auto &[snapshot, file, _] = entry;
        add_handle(file.get());
        return DeviceFsSource{
            .name{SnapshotImageNameSequence(snapshot)},
            .source{devicefs::rpc::kHandleDevicePrefix,
                devicefs::TextualNumber{
                    std::bit_cast<std::uintptr_t>(file.get())}}};
    });
    const auto sources_second = devicefs::CreateObjects(
        sources_first.data() + sources_first.size(), dirty_maps,
        [&add_handle](const auto &entry) {
        const auto &[snapshot, handle] = entry;
        add_handle(handle);
        return DeviceFsSource{
            .name{SnapshotImageNameSequence(snapshot), ".dirty.bitmap"sv},
            .source{devicefs::rpc::kFileHandleDevicePrefix,
                devicefs::TextualNumber{
                    std::bit_cast<std::uintptr_t>(handle)}}};
    });

    return DeviceFsProcess{DeviceFsStartRequest{
        .sources = std::span{source_buffer.get(), source_count},
        .inherited_handles = std::span{inherited_handles.get(), handle_count},
        .mount_target = DeviceFsProcess::kMountTarget,
        .read_user = read_user,
        .expose_known_data_map = use_known_data_map,
    }};
}

} // namespace devicefs_child_detail

[[nodiscard]] auto DeviceFsProcess::WaitUntilReady(
    const HANDLE cancellation_event) const {
    const auto deadline =
        std::chrono::steady_clock::now() + DeviceFsProcess::kStartTimeout;
    while (true) {
        if (WaitForProcess(process.hProcess,
                DeviceFsProcess::kPollInterval)) {
            throw std::runtime_error(std::format(
                "devicefs exited with code {} before creating readiness path '{}'",
                ProcessExitCode(process.hProcess),
                Transcode<std::string>(readiness_path.native())));
        }
        if (wil::event_is_signaled(cancellation_event)) {
            return false;
        }
        auto exists_error = std::error_code{};
        if (std::filesystem::is_regular_file(readiness_path, exists_error)) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(std::format(
                "devicefs did not create readiness path '{}' within {} seconds",
                Transcode<std::string>(readiness_path.native()),
                std::chrono::duration_cast<std::chrono::seconds>(
                    DeviceFsProcess::kStartTimeout).count()));
        }
    }
}

namespace devicefs_process_detail {

[[nodiscard]] auto WaitForExitOrCancellation(
    const HANDLE cancellation_event, const auto &...devicefs_processes) {
    struct DeviceFsExit {
        std::size_t process_index;
        DWORD exit_code;
    };
    using Return = std::optional<DeviceFsExit>;
    const auto result = wil::wait_any_failfast(
        devicefs_processes.process.hProcess..., cancellation_event);
    const auto process_index =
        CompileTimeCast<std::size_t>(result.status() - WAIT_OBJECT_0);
    if (process_index < sizeof...(devicefs_processes)) {
        const auto processes = std::array{std::cref(devicefs_processes)...};
        return Return{std::in_place, process_index,
            ProcessExitCode(processes[process_index].get().process.hProcess)};
    }
    return Return{};
}

} // namespace devicefs_process_detail

[[nodiscard]] auto DeviceFsProcess::WaitForExit(
    const HANDLE cancellation_event,
    const auto &...other_devicefs_processes) const {
    return devicefs_process_detail::WaitForExitOrCancellation(
        cancellation_event, *this, other_devicefs_processes...);
}

template <class ExitConfirmation = std::monostate>
class BasicDeviceFsChild {
public:
    explicit BasicDeviceFsChild(auto &&...args)
        requires ((!std::is_same_v<std::remove_cvref_t<decltype(args)>,
            DeviceFsStartRequest> && ...) && requires {
                devicefs_child_detail::StartDeviceFs(
                    std::forward<decltype(args)>(args)...);
            }) :
        devicefs_{devicefs_child_detail::StartDeviceFs(
            std::forward<decltype(args)>(args)...)} {}

    explicit BasicDeviceFsChild(const DeviceFsStartRequest &request,
        ExitConfirmation exit_confirmed = ExitConfirmation{}) :
        devicefs_{request},
        exit_confirmed_{exit_confirmed} {
        if constexpr (!std::same_as<ExitConfirmation, std::monostate>) {
            exit_confirmed_.get() = false;
        }
    }

    explicit BasicDeviceFsChild(DeviceFsProcess &&process) noexcept :
        devicefs_{std::move(process)} {}

    BasicDeviceFsChild(const BasicDeviceFsChild &) = delete;
    BasicDeviceFsChild(BasicDeviceFsChild &&) = delete;
    auto operator=(const BasicDeviceFsChild &) -> BasicDeviceFsChild & = delete;
    auto operator=(BasicDeviceFsChild &&) -> BasicDeviceFsChild & = delete;

    ~BasicDeviceFsChild() {
        // Windows must still be able to read a VHDX's backing file to detach
        // the disk. If detachment failed, terminating DeviceFs could make the
        // disk impossible to detach without rebooting. To mitigate that, we
        // signal an event to request that DeviceFs shut down gracefully. After
        // receipt of that signal, DeviceFs waits for open files to close before
        // exiting, so that it can continue serving a VHDX that remains attached.
        devicefs_.stop_event.SetEvent();
        if (WaitForProcess(devicefs_.process.hProcess,
                DeviceFsProcess::kShutdownTimeout)) {
            if constexpr (!std::same_as<ExitConfirmation, std::monostate>) {
                exit_confirmed_.get() = true;
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

    auto Process() const noexcept -> const auto & {
        return devicefs_;
    }

private:
    DeviceFsProcess devicefs_;
    [[msvc::no_unique_address]] ExitConfirmation exit_confirmed_;
};

using DeviceFsChild = BasicDeviceFsChild<>;

} // namespace internal
