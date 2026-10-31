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

import std;
import devicefs.terminal.safecast;
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

[[nodiscard]] auto QueryDeviceLength(const HANDLE device)
    -> std::expected<std::uint64_t, DWORD> {
    auto length = GET_LENGTH_INFORMATION{};
    if (const auto status = Ioctl(device, IOCTL_DISK_GET_LENGTH_INFO,
            &length, sizeof(length));
        status != ERROR_SUCCESS) {
        return std::unexpected{status};
    }
    return terminal::FailFastCast<std::uint64_t>(length.Length.QuadPart);
}

[[nodiscard]] auto QueryVolumeFileSystem(const HANDLE device)
    -> std::expected<std::wstring, DWORD> {
    // The documentation for `GetVolumeInformationByHandleW` specifies that the
    // maximum buffer size for a filesystem is `MAX_PATH + 1` characters but the
    // documentation does not specify whether the function writes a null
    // terminator into the buffer.
    constexpr auto buffer_length = MAX_PATH + 1;
    auto buffer = std::wstring(buffer_length, L'\0');
    if (!GetVolumeInformationByHandleW(device, nullptr, 0, nullptr, nullptr,
            nullptr, buffer.data(), buffer_length)) {
        return std::unexpected{GetLastError()};
    }
    // Remove any suffix starting with the first null character.
    buffer.erase(std::ranges::find(buffer, L'\0'), buffer.end());
    return buffer;
}

namespace detail {

using GeometryType = std::expected<std::pair<UINT64, DWORD>, DWORD>;

template <class Buffer, auto Code>
auto QueryVolumeGeometry(const HANDLE device) -> GeometryType {
    // We use `Ioctl` rather than `GetDiskSpaceInformationW` because
    // `GetDiskSpaceInformationW` has been observed to fail for ReFS volumes.
    auto volume = Buffer{};
    if (const auto status = Ioctl(device, Code, &volume, sizeof(volume));
        status != ERROR_SUCCESS) {
        return std::unexpected{status};
    }
    return std::pair{
        devicefs::terminal::FailFastCast<UINT64>(
            volume.TotalClusters.QuadPart),
        volume.BytesPerCluster};
}

} // namespace detail

auto QueryVolumeGeometry(const HANDLE device,
    const std::optional<std::wstring_view> file_system = {})
    -> detail::GeometryType {
    if (!file_system) {
        const auto file_system_ = QueryVolumeFileSystem(device);
        if (!file_system_) {
            return std::unexpected{file_system_.error()};
        }
        return QueryVolumeGeometry(device, *file_system_);
    }
    if (file_system == L"ReFS") {
        return detail::QueryVolumeGeometry<
            REFS_VOLUME_DATA_BUFFER, FSCTL_GET_REFS_VOLUME_DATA>(device);
    }
    // Filesystems other than ReFs are assumed to be NTFS.
    // If that assumption is wrong, this call will naturally fail.
    return detail::QueryVolumeGeometry<
        NTFS_VOLUME_DATA_BUFFER, FSCTL_GET_NTFS_VOLUME_DATA>(device);
}

} // namespace devicefs::filesystem
