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

export module devicefs.svi_extents;

import std;
import devicefs.filesystem.ioctl;
import devicefs.filesystem.mount_manager;
import devicefs.terminal.safecast;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;
import devicefs.vss_block_descriptors;

namespace devicefs::svi {

export struct SviExtentsOptions {
    // The requested alignment in bytes; must be nonzero.
    std::uint64_t block_size = devicefs::vss::kBlockSize;
    bool vss_only = false;
    // If false, retain only aligned blocks wholly contained in an extent.
    bool round_up = true;
};

namespace detail {

constexpr auto kShareMode =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr auto kSecurityFlags =
    SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION;

[[nodiscard]] auto OpenObject(const std::filesystem::path &path) {
    auto result = wil::unique_hfile{CreateFileW(
        path.c_str(), GENERIC_READ, kShareMode, nullptr, OPEN_EXISTING,
        kSecurityFlags |
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr)};
    if (!result) {
        WinError("could not open System Volume Information object '{}'",
            std::wstring_view{path.native()});
    }
    return result;
}

[[nodiscard]] auto QueryClusterSize(
    const HANDLE device,
    const std::filesystem::path &path) {
    auto ntfs = NTFS_VOLUME_DATA_BUFFER{};
    if (const auto status = filesystem::Ioctl(
            device, FSCTL_GET_NTFS_VOLUME_DATA, &ntfs, sizeof(ntfs));
        status != ERROR_SUCCESS) {
        WinError("FSCTL_GET_NTFS_VOLUME_DATA failed for '{}'",
            std::wstring_view{path.native()}, ExplicitWin32Error{status});
    }
    return CompileTimeCast<std::uint64_t>(ntfs.BytesPerCluster);
}

auto ThrowIfFileInfoFailed(
    const HRESULT result,
    const std::string_view operation,
    const std::filesystem::path &path) -> void {
    if (SUCCEEDED(result)) {
        return;
    }
    WinError("{} for '{}'", operation, std::wstring_view{path.native()},
        ExplicitHresult{result});
}

[[nodiscard]] constexpr auto IsVssStorageFile(const std::wstring_view name)
    noexcept -> bool {
    // VSS uses this GUID for its control file and as the suffix of each
    // shadow-copy storage filename.
    return name.ends_with(L"{3808876b-c176-4e48-b7ae-04046e6cc752}");
}

template <class RangeAdder>
class SviExtentReader {
public:
    SviExtentReader(
        const std::uint64_t cluster_size,
        RangeAdder &add_range,
        const devicefs::svi::SviExtentsOptions options) noexcept
        : cluster_size_{cluster_size}, add_range_{add_range},
          options_{options} {}

    auto ReadTreeBlockOffsets(const std::filesystem::path &root) -> void {
        auto root_handle = OpenObject(root);
        if (options_.vss_only) {
            EnumerateDirectory(root, root_handle.get());
            return;
        }
        auto attributes = FILE_ATTRIBUTE_TAG_INFO{};
        ThrowIfFileInfoFailed(
            wil::GetFileInfoNoThrow<FileAttributeTagInfo>(
                root_handle.get(), &attributes),
            "could not query System Volume Information attributes", root);
        ReadObject(root, attributes.FileAttributes, root_handle.get());
    }

private:
    auto AddRange(
        const std::uint64_t starting_cluster,
        const std::uint64_t cluster_count) -> void {
        const auto start = starting_cluster * cluster_size_;
        const auto end =
            start + (cluster_count * cluster_size_);
        const auto block_size = options_.block_size;
        const auto first_block = start / block_size +
            (!options_.round_up && ((start % block_size) != 0));
        const auto past_last_block = end / block_size +
            (options_.round_up && ((end % block_size) != 0));
        if (first_block < past_last_block) {
            std::invoke(add_range_, first_block * block_size,
                past_last_block * block_size);
        }
    }

    auto ReadExtents(
        const std::filesystem::path &path,
        const HANDLE object) -> void {
        auto starting_vcn = LARGE_INTEGER{};
        while (true) {
            auto input = STARTING_VCN_INPUT_BUFFER{
                .StartingVcn = starting_vcn,
            };
            auto output = RETRIEVAL_POINTERS_BUFFER{};
            auto returned = DWORD{};
            const auto completed = DeviceIoControl(object,
                FSCTL_GET_RETRIEVAL_POINTERS,
                &input, sizeof(input), &output, sizeof(output),
                &returned, nullptr);
            const auto error = completed ? DWORD{ERROR_SUCCESS} : GetLastError();
            if (!completed && (error == ERROR_HANDLE_EOF)) {
                return;
            }
            if (!completed && (error != ERROR_MORE_DATA)) {
                WinError(
                    "could not retrieve System Volume Information extents for '{}'",
                    std::wstring_view{path.native()},
                    ExplicitWin32Error{error});
            }
            const auto current_vcn = output.StartingVcn.QuadPart;
            const auto next_vcn = output.Extents[0].NextVcn.QuadPart;
            const auto lcn = output.Extents[0].Lcn.QuadPart;
            if (lcn >= 0) {
                AddRange(
                    devicefs::terminal::FailFastCast<std::uint64_t>(lcn),
                    devicefs::terminal::FailFastCast<std::uint64_t>(
                        next_vcn - current_vcn));
            }

            if (completed) {
                return;
            }
            starting_vcn = output.Extents[0].NextVcn;
        }
    }

    auto ReadNamedDataStreams(
        const std::filesystem::path &path,
        const HANDLE object) -> void {
        auto streams = wistd::unique_ptr<FILE_STREAM_INFO>{};
        ThrowIfFileInfoFailed(
            wil::GetFileInfoNoThrow<FileStreamInfo>(object, streams),
            "could not enumerate System Volume Information streams", path);
        for (const auto &stream :
            wil::create_next_entry_offset_iterator(streams.get())) {
            const auto name = std::wstring_view{
                &stream.StreamName[0],
                stream.StreamNameLength /
                    sizeof(std::wstring_view::value_type),
            };
            // ReadObject already measured the unnamed data stream through the
            // original object handle.
            if (name == L"::$DATA") {
                continue;
            }

            const auto stream_path = std::filesystem::path{
                std::format(L"{}{}", path.native(), name)};
            ReadExtents(stream_path, OpenObject(stream_path).get());
        }
    }

    auto EnumerateDirectory(
        const std::filesystem::path &path,
        const HANDLE directory) -> void {
        while (true) {
            auto entries = wistd::unique_ptr<FILE_FULL_DIR_INFO>{};
            ThrowIfFileInfoFailed(
                wil::GetFileInfoNoThrow<FileFullDirectoryInfo>(
                    directory, entries),
                "could not enumerate System Volume Information", path);
            if (!entries) {
                return;
            }

            for (const auto &entry :
                wil::create_next_entry_offset_iterator(entries.get())) {
                const auto name = std::wstring_view{
                    &entry.FileName[0],
                    entry.FileNameLength /
                        sizeof(std::wstring_view::value_type),
                };
                if ((name == L".") || (name == L"..")) {
                    continue;
                }

                if (options_.vss_only &&
                    (((entry.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) ||
                        !IsVssStorageFile(name))) {
                    continue;
                }

                const auto child_path = path / name;
                auto child = OpenObject(child_path);
                ReadObject(child_path, entry.FileAttributes, child.get());
            }
        }
    }

    auto ReadObject(
        const std::filesystem::path &path,
        const DWORD attributes,
        const HANDLE object) -> void {
        ReadExtents(path, object);
        ReadNamedDataStreams(path, object);
        if (((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) &&
            ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)) {
            EnumerateDirectory(path, object);
        }
    }

    const std::uint64_t cluster_size_;
    RangeAdder &add_range_;
    const devicefs::svi::SviExtentsOptions options_;
};

} // namespace detail

// Pass each aligned byte range [first, limit) to `add_range`.
// Ranges may overlap and are not sorted.
// `volume_path` may be supplied when the volume's pathname is already known.
export template <class RangeAdder>
    requires std::invocable<RangeAdder &, std::uint64_t, std::uint64_t>
auto EnumerateBlockRanges(const HANDLE volume, RangeAdder &&add_range,
    const std::filesystem::path &volume_path = {},
    const SviExtentsOptions options = {}) -> void {
    const auto device_path = volume_path.empty()
        ? devicefs::filesystem::QueryVolumePath(volume) : volume_path;
    const auto cluster_size = detail::QueryClusterSize(volume, device_path);
    const auto root = device_path / L"System Volume Information";
    detail::SviExtentReader{cluster_size, add_range, options}.
        ReadTreeBlockOffsets(root);
}

// Return a bitmap of SVI extent data. A set bit indicates that at least some
// portion of the corresponding region contains SVI data.
//
// Precondition: the process has the backup reading privilege.
export [[nodiscard]] auto ReadBlockOffsets(
    const std::string_view snapshot_device)
    -> devicefs::vss::BlockOffsetSet {
    const auto device_path = std::filesystem::path{snapshot_device};
    auto device = wil::unique_hfile{CreateFileW(
        device_path.c_str(), GENERIC_READ, detail::kShareMode, nullptr,
        OPEN_EXISTING, detail::kSecurityFlags, nullptr)};
    if (!device) {
        WinError("could not open SVI snapshot volume '{}'",
            std::wstring_view{device_path.native()});
    }

    auto block_offsets = devicefs::vss::BlockOffsetSet{};
    // The successful insertion of ranges into `block_offsets` relies on the
    // truth of the following proposition.
    static_assert(decltype(block_offsets)::block_size{} ==
        SviExtentsOptions{}.block_size);

    EnumerateBlockRanges(device.get(),
        [&block_offsets](const auto first, const auto limit) {
            block_offsets.insert_range(first, limit);
        }, device_path);
    return block_offsets;
}

} // namespace devicefs::svi
