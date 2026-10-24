// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <devicefs/strsafe_compat.h>

module devicefs.windows_block_device;

import std;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;
import devicefs.allocation;
import devicefs.stream_writer;

namespace devicefs::filesystem_internal {

ThreadLocalResourceAggregate::ThreadLocalResourceAggregate() noexcept :
    retained_buffer{[] static noexcept {
        static auto alignment_once = std::once_flag{};
        std::call_once(alignment_once,[] static noexcept {
            if (WindowsBlockDeviceSettings::read_buffer_alignment ==
                    std::align_val_t{}) {
                auto information = SYSTEM_INFO{};
                GetSystemInfo(&information);
                WindowsBlockDeviceSettings::read_buffer_alignment =
                    std::align_val_t{information.dwPageSize};
            }
        });
        auto buffer = NewAlignedBuffer(
            WindowsBlockDeviceSettings::kRetainedReadBufferSize,
            WindowsBlockDeviceSettings::read_buffer_alignment);
        if (!buffer) {
            devicefs::WriteToStream(devicefs::stderr,
                "devicefs: failed to allocate retained read buffer "
                "of {} bytes with {}-byte alignment\n",
                WindowsBlockDeviceSettings::kRetainedReadBufferSize,
                std::to_underlying(
                    WindowsBlockDeviceSettings::read_buffer_alignment));
            std::_Exit(ERROR_NOT_ENOUGH_MEMORY);
        }
        return buffer;
    }()},
    event{[] static noexcept {
        auto event = wil::unique_event_nothrow{};
        if (!event.try_create(wil::EventOptions::ManualReset, nullptr)) {
            const auto error = GetLastError();
            devicefs::WriteToStream(devicefs::stderr,
                "devicefs: failed to create read completion event: {}\n",
                TryConstructWinError("", ExplicitWin32Error{error}));
            std::_Exit(error);
        }
        return event;
    }()} {}

auto ErrorClosure::operator()(const DWORD error) const noexcept -> NTSTATUS {
    devicefs::WriteToStream(
        devicefs::stderr,
        L"devicefs: read failed for '{}' at offset 0x{:x} "
        L"for {} bytes: {}\n",
        std::wstring_view{filename.native()}, offset, wanted,
        TryConstructWinError("", ExplicitWin32Error{error}));
    return FspNtStatusFromWin32(error);
}

} // namespace devicefs::filesystem_internal
