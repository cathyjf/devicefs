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

export module devicefs.filesystem.ioctl;

import <devicefs/windows_imports.h>;

export namespace devicefs::filesystem {

__pragma(warning(push))
__pragma(warning(disable : 26477, justification :
    "The analyzer suggests using `nullptr` rather than 0 or `NULL`. However, "
    "this function already correctly uses `nullptr` for null pointers."))
_Success_(return == ERROR_SUCCESS)
[[nodiscard]] auto Ioctl(const HANDLE device, const DWORD code,
    _Out_writes_bytes_opt_(output_size) void *const output,
    const DWORD output_size,
    _In_reads_bytes_opt_(input_size) void *const input = nullptr,
    const DWORD input_size = 0,
    _Out_opt_ DWORD *const bytes_returned = nullptr) -> DWORD {
    auto event = wil::unique_event_nothrow{};
    if (!event.try_create(wil::EventOptions::ManualReset, nullptr)) {
        return GetLastError();
    }

    auto operation = OVERLAPPED{.hEvent = event.get()};
    auto returned = DWORD{};
    auto *const result_size =
        (bytes_returned == nullptr) ? &returned : bytes_returned;
    if (DeviceIoControl(device, code, input, input_size,
            output, output_size, result_size, &operation)) {
        return ERROR_SUCCESS;
    }
    const auto error = GetLastError();
    if (error != ERROR_IO_PENDING) {
        return error;
    }
    if (!GetOverlappedResult(device, &operation, result_size, TRUE)) {
        return GetLastError();
    }
    return ERROR_SUCCESS;
}
__pragma(warning(pop))

} // namespace devicefs::filesystem
