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
#include <winioctl.h>

// As a result of an apparent compiler defect, when the below WIL headers are
// imported (instead of included), MSVC++ is able to find the declaration of
// `StringValidateDestW` but not the definition of it, even though both the
// declaration and definition are contained within `strsafe.h` and the compiler
// should presumably either find both or neither. To work around this issue, we
// explicitly include `strsafe.h`.
#include <strsafe.h>

module devicefs.supervisor.native_backup;

import std;
import <wil/resource.h>;
import <wil/safecast.h>;
import :internal;
import devicefs.common;
import devicefs.supervisor.configuration;
import devicefs.supervisor.embedded_artifacts;
import devicefs.supervisor.installation;

using namespace std::chrono_literals;

namespace internal {

[[nodiscard]] auto CancellationRequested(
    const HANDLE cancellation_event) {
    const auto result = WaitForSingleObject(cancellation_event, 0);
    if (result == WAIT_FAILED) {
        WinError("could not inspect device-to-FIFO cancellation");
    }
    return result == WAIT_OBJECT_0;
}

class DeviceStream {
  public:
    explicit DeviceStream(std::wstring_view device);
    auto WriteTo(HANDLE destination, HANDLE cancellation_event) -> void;

  private:
    wil::unique_hfile source_;
    std::uint64_t size_ = 0;
    DWORD buffer_size_ = 0;
    wil::unique_virtualalloc_ptr<BYTE> buffer_;
};

DeviceStream::DeviceStream(const std::wstring_view device) {
    const auto device_path = std::wstring{device};
    source_.reset(CreateFileW(
        device_path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
        nullptr));
    if (!source_) {
        WinError("could not open the device");
    }

    auto length = GET_LENGTH_INFORMATION{};
    auto returned = DWORD{};
    if (!DeviceIoControl(source_.get(), IOCTL_DISK_GET_LENGTH_INFO,
            nullptr, 0, &length, sizeof(length),
            &returned, nullptr)) {
        WinError("IOCTL_DISK_GET_LENGTH_INFO failed for the device");
    }
    if (length.Length.QuadPart < 0) {
        throw std::runtime_error(
            "IOCTL_DISK_GET_LENGTH_INFO returned an invalid device length");
    }
    size_ = wil::safe_cast<std::uint64_t>(length.Length.QuadPart);

    auto geometry = DISK_GEOMETRY{};
    if (!DeviceIoControl(source_.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY,
            nullptr, 0, &geometry, sizeof(geometry),
            &returned, nullptr)) {
        WinError("IOCTL_DISK_GET_DRIVE_GEOMETRY failed for the device");
    }
    if ((geometry.BytesPerSector == 0) ||
        ((size_ % geometry.BytesPerSector) != 0)) {
        throw std::runtime_error(
            "device length is not a multiple of its sector size");
    }

    if (!DeviceIoControl(source_.get(), FSCTL_ALLOW_EXTENDED_DASD_IO,
            nullptr, 0, nullptr, 0, &returned, nullptr)) {
        const auto error = GetLastError();
        const auto code = std::error_code(
            std::bit_cast<int>(error), std::system_category());
        std::wcerr
            << L"backup-supervisor: warning: FSCTL_ALLOW_EXTENDED_DASD_IO "
               L"failed for '"
            << device_path << L"': " << code.message().c_str() << L'\n';
    }

    constexpr auto kDesiredBufferSize = DWORD{4 * 1024 * 1024};
    buffer_size_ = geometry.BytesPerSector > kDesiredBufferSize
        ? geometry.BytesPerSector
        : kDesiredBufferSize -
            (kDesiredBufferSize % geometry.BytesPerSector);
    buffer_.reset(static_cast<BYTE *>(VirtualAlloc(
        nullptr, buffer_size_, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)));
    if (!buffer_) {
        WinError("could not allocate the device transfer buffer");
    }
}

auto DeviceStream::WriteTo(
    const HANDLE destination,
    const HANDLE cancellation_event) -> void {
    auto remaining = size_;
    while (remaining != 0) {
        if (CancellationRequested(cancellation_event)) {
            throw std::runtime_error(
                "the device-to-FIFO transfer was cancelled");
        }
        const auto requested = remaining < buffer_size_
            ? wil::safe_cast<DWORD>(remaining)
            : buffer_size_;
        auto read = DWORD{};
        if (!ReadFile(source_.get(), buffer_.get(), requested,
                &read, nullptr)) {
            WinError("could not read the device");
        }
        if (read != requested) {
            WinError("could not read a complete device block",
                ExplicitWin32Error{ERROR_READ_FAULT});
        }
        if (CancellationRequested(cancellation_event)) {
            throw std::runtime_error(
                "the device-to-FIFO transfer was cancelled");
        }
        auto written = DWORD{};
        if (!WriteFile(destination, buffer_.get(), read,
                &written, nullptr)) {
            WinError("could not write device data");
        }
        if (written != read) {
            WinError("could not write a complete device block",
                ExplicitWin32Error{ERROR_WRITE_FAULT});
        }
        remaining -= read;
    }
}

[[nodiscard]] auto TryStopWslFifoRelay(
    WslProcess &relay,
    const std::wstring_view pid_file,
    const std::wstring_view stop_file) noexcept {
    try {
        constexpr auto kTermTimeout = 45s;
        constexpr auto kKillTimeout = 30s;
        if (WaitForProcess(relay.process.hProcess, 0ms)) {
            return true;
        }
        TrySendWslBackupSignal(pid_file, stop_file, WslBackupSignal::Term);
        if (!WaitForProcess(relay.process.hProcess, kTermTimeout)) {
            TrySendWslBackupSignal(pid_file, stop_file, WslBackupSignal::Kill);
            if (!WaitForProcess(relay.process.hProcess, kKillTimeout)) {
                throw std::runtime_error(
                    "the device-to-FIFO relay did not exit after the KILL request");
            }
        }
        return true;
    } catch (const std::exception &error) {
        TryWriteError("device-to-FIFO relay cleanup failed", error);
        return false;
    }
}

[[nodiscard]] auto FinishWslFifoRelay(StartedWslFish &relay) {
    relay.standard_input.reset();
    relay.standard_output.reset();
    return FinishWsl(relay.process);
}

auto TryFinishWslFifoRelay(StartedWslFish &relay) noexcept {
    try {
        static_cast<void>(FinishWslFifoRelay(relay));
    } catch (const std::exception &error) {
        TryWriteError("device-to-FIFO relay cleanup failed", error);
    }
}

} // namespace internal

[[nodiscard]] auto RunDeviceToFifo(
    const HANDLE cancellation_event,
    const std::wstring_view device,
    const std::wstring_view fifo_path) -> int {
    if (internal::CancellationRequested(cancellation_event)) {
        return kCancelledExitCode;
    }
    auto device_stream = internal::DeviceStream{device};
    const auto control_path = std::format(
        L"/tmp/devicefs-fifo-{}", internal::UniqueName());
    const auto pid_file = std::format(L"{}.pid", control_path);
    const auto stop_file = std::format(L"{}.stop", control_path);
    const auto arguments = std::array<std::wstring_view, 3>{
        fifo_path, pid_file, stop_file};
    auto worker = std::thread{};
    auto relay = [&] {
        const auto persistent = ResolvePersistentPaths();
        const auto configuration =
            ReadBackupConfiguration(persistent.configuration);
        return internal::StartWslFishProcess(configuration, arguments);
    }();
    auto cleanup = wil::scope_exit([&] {
        auto stopped = false;
        if (worker.joinable()) {
            // Stop the relay before releasing its producer so failure cannot
            // appear to the FIFO reader as an ordinary short EOF.
            stopped = internal::TryStopWslFifoRelay(
                relay.process, pid_file, stop_file);
            static_cast<void>(
                CancelSynchronousIo(worker.native_handle()));
            worker.join();
        } else {
            relay.standard_input.reset();
            stopped = internal::TryStopWslFifoRelay(
                relay.process, pid_file, stop_file);
        }
        relay.standard_input.reset();
        if (stopped) {
            internal::TryFinishWslFifoRelay(relay);
        }
    });
    relay.process.standard_error.emplace(
        std::move(relay.standard_error), GetStdHandle(STD_ERROR_HANDLE));

    auto operation = std::packaged_task<void()>([&] {
        // Fish cannot observe the stop file until it has evaluated the complete
        // NUL-terminated program.
        const auto program = DeviceToFifoProgram();
        const auto program_size =
            wil::safe_cast<DWORD>(program.size_bytes());
        auto written = DWORD{};
        if (!WriteFile(relay.standard_input.get(), program.data(),
                program_size, &written, nullptr)) {
            WinError("could not write the embedded device-to-FIFO relay");
        }
        if (written != program_size) {
            WinError(
                "could not write the complete embedded device-to-FIFO relay",
                ExplicitWin32Error{ERROR_WRITE_FAULT});
        }
        constexpr auto kProgramTerminator = char{0};
        if (!WriteFile(relay.standard_input.get(), &kProgramTerminator,
                1, &written, nullptr)) {
            WinError("could not terminate the embedded device-to-FIFO relay");
        }
        if (written != 1) {
            WinError(
                "could not write the complete device-to-FIFO relay terminator",
                ExplicitWin32Error{ERROR_WRITE_FAULT});
        }
        if (internal::CancellationRequested(cancellation_event)) {
            throw std::runtime_error(
                "the device-to-FIFO transfer was cancelled");
        }
        auto ready = char{};
        auto read = DWORD{};
        if (!ReadFile(relay.standard_output.get(), &ready, 1,
                &read, nullptr)) {
            WinError("could not read device-to-FIFO relay readiness");
        }
        if ((read != 1) || (ready != 'R')) {
            throw std::runtime_error(
                "the device-to-FIFO relay returned an invalid readiness marker");
        }
        if (internal::CancellationRequested(cancellation_event)) {
            throw std::runtime_error(
                "the device-to-FIFO transfer was cancelled");
        }
        relay.process.standard_output.emplace(
            std::move(relay.standard_output),
            GetStdHandle(STD_OUTPUT_HANDLE));
        std::wcout << L"Device-to-FIFO relay ready: " << fifo_path << L'\n'
                   << std::flush;
        device_stream.WriteTo(relay.standard_input.get(), cancellation_event);
        relay.standard_input.reset();
    });
    auto operation_result = operation.get_future();
    worker = std::thread{std::move(operation)};

    constexpr auto kPollInterval = 100ms;
    while (operation_result.wait_for(kPollInterval) !=
        std::future_status::ready) {
        if (!internal::CancellationRequested(cancellation_event)) {
            continue;
        }
        return kCancelledExitCode;
    }

    if (internal::CancellationRequested(cancellation_event)) {
        return kCancelledExitCode;
    }
    operation_result.get();
    worker.join();

    constexpr auto kProcessPollInterval = 100ms;
    while (!internal::WaitForProcess(
            relay.process.process.hProcess, kProcessPollInterval)) {
        if (internal::CancellationRequested(cancellation_event)) {
            return kCancelledExitCode;
        }
    }

    if (internal::CancellationRequested(cancellation_event)) {
        return kCancelledExitCode;
    }
    cleanup.release();
    const auto exit_code = internal::FinishWslFifoRelay(relay);
    if (exit_code != 0) {
        throw std::runtime_error(std::format(
            "the device-to-FIFO relay exited with code {}", exit_code));
    }
    return 0;
}
