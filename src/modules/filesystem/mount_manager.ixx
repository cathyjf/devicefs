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

export module devicefs.filesystem.mount_manager;

import std;
import <devicefs/common.h>;
import <devicefs/malloca.h>;
import <devicefs/windows_imports.h>;
import devicefs.filesystem.ioctl;

export namespace devicefs::filesystem {

// Retrieve the mount manager's opaque device ID through `device`.
[[nodiscard]] auto QueryMountManagerId(const HANDLE device)
    -> std::expected<std::vector<BYTE>, DWORD> {
    // Windows SDKs 26100 and 28000 omit the mount manager query declarations
    // supplied by the WDK in `mountdev.h`. Declaring them locally allows
    // DeviceFs to issue the query without requiring the WDK to build.
    constexpr auto kQueryUniqueId =
        CTL_CODE('M', 0, METHOD_BUFFERED, FILE_ANY_ACCESS);
    struct {
        USHORT length;
        BYTE bytes[1];
    } header{};
    // The documented minimum output size is `sizeof(MOUNTDEV_UNIQUE_ID)`,
    // so the length probe uses the full structure even though it only
    // needs `UniqueIdLength`.
    // https://learn.microsoft.com/windows-hardware/drivers/ddi/mountdev/ni-mountdev-ioctl_mountdev_query_unique_id#output-buffer-length
    const auto probe_error = Ioctl(
        device, kQueryUniqueId, &header, sizeof(header));
    if ((probe_error != ERROR_SUCCESS) && (probe_error != ERROR_MORE_DATA)) {
        return std::unexpected{probe_error};
    }
    constexpr auto kIdOffset =
        CompileTimeCast<DWORD, offsetof(decltype(header), bytes)>();
    const auto output_size = std::max(
        CompileTimeCast<DWORD, sizeof(header)>(), kIdOffset + header.length);
    const auto output = NEW_MALLOCA_BUFFER(BYTE, output_size);
    if (!output) {
        return std::unexpected{ERROR_NOT_ENOUGH_MEMORY};
    }

    if (const auto error =
            Ioctl(device, kQueryUniqueId, output.get(), output_size);
        error != ERROR_SUCCESS) {
        return std::unexpected{error};
    }
    const auto length =
        std::start_lifetime_as<decltype(header)>(output.get())->length;
    return std::span{
        std::start_lifetime_as_array<BYTE>(
            output.get() + kIdOffset, length),
        length} | std::ranges::to<std::vector<BYTE>>();
}

// Return a Win32 GLOBALROOT path for a volume's device name.
[[nodiscard]] auto QueryVolumePath(const HANDLE volume)
    -> std::filesystem::path {
    const auto query = [volume](void *const output, const DWORD size,
        const bool probe = false) {
        constexpr auto kQueryDeviceName =
            CTL_CODE('M', 2, METHOD_BUFFERED, FILE_ANY_ACCESS);
        const auto error = Ioctl(volume, kQueryDeviceName, output, size);
        if ((error != ERROR_SUCCESS) &&
            (!probe || (error != ERROR_MORE_DATA))) {
            WinError("failed to query the device name for volume handle {}",
                volume, ExplicitWin32Error{error});
        }
    };

    // MOUNTDEV_NAME is supplied by the WDK rather than by the Windows SDK.
    struct {
        USHORT name_length;
        WCHAR name[1];
    } header{};
    query(&header, sizeof(header), true);

    using Character = std::remove_extent_t<decltype(header.name)>;
    constexpr auto kNameOffset =
        CompileTimeCast<DWORD, offsetof(decltype(header), name)>();
    constexpr auto prefix = std::basic_string_view<Character>{
        LR"(\\?\GLOBALROOT)"};
    constexpr auto kPrefixBytes = prefix.size() * sizeof(Character);
    constexpr auto kExtraBytes = kPrefixBytes - kNameOffset;

    const auto output_size = kNameOffset + header.name_length;
    const auto buffer = NEW_MALLOCA_BUFFER(std::byte,
        kExtraBytes + output_size);
    if (!buffer) {
        throw std::bad_alloc{};
    }
    auto *const output = buffer.get() + kExtraBytes;
    query(output, output_size);

    const auto name_length =
        std::start_lifetime_as<decltype(header)>(output)->name_length;
    // Once its length has been read, the query header can be overwritten
    // by the end of the prefix, leaving the returned name directly after it.
    std::memcpy(buffer.get(), prefix.data(), kPrefixBytes);
    const auto characters = prefix.size() + name_length / sizeof(Character);
    const auto *const path =
        std::start_lifetime_as_array<Character>(buffer.get(), characters);
    return std::filesystem::path{std::wstring_view{path, characters}};
}

} // namespace devicefs::filesystem
