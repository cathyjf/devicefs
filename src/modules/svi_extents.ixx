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
    // Emit ranges aligned to this size in bytes.
    std::uint64_t block_size = devicefs::vss::kBlockSize;

    // The cluster size of the volume, if known. If not specified, it will
    // be queried from the volume.
    std::optional<std::uint64_t> cluster_size;

    // If true, visit only objects related to VSS.
    // If false, visit all objects under System Volume Information.
    bool vss_only = false;

    // If true, emit all ranges that contain any portion of a visited extent.
    // If false, emit only ranges that lie entirely within a visited extent.
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
        WinError("failed to open System Volume Information object '{}'",
            std::wstring_view{path.native()});
    }
    return result;
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
            "failed to query System Volume Information attributes", root);
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
        using Output = RETRIEVAL_POINTERS_BUFFER;
        using Extent = std::remove_extent_t<decltype(Output::Extents)>;
        constexpr auto kExtentCapacity = std::size_t{128};
        constexpr auto kOutputSize = offsetof(Output, Extents) +
            kExtentCapacity * sizeof(Extent);
        __pragma(warning(suppress : 26494, justification :
            "The array `output` is intentionally uninitialized."))
        alignas(Output) /* [[indeterminate]] */
            std::array<std::byte, kOutputSize> output;
        auto starting_vcn = LARGE_INTEGER{};
        while (true) {
            auto input = STARTING_VCN_INPUT_BUFFER{
                .StartingVcn = starting_vcn,
            };
            auto returned = DWORD{};
            const auto completed = DeviceIoControl(object,
                FSCTL_GET_RETRIEVAL_POINTERS,
                &input, sizeof(input), output.data(), sizeof(output),
                &returned, nullptr);
            const auto error = completed ? DWORD{ERROR_SUCCESS} : GetLastError();
            if (!completed && (error == ERROR_HANDLE_EOF)) {
                return;
            }
            if (!completed && (error != ERROR_MORE_DATA)) {
                WinError(
                    "failed to retrieve System Volume Information extents for '{}'",
                    std::wstring_view{path.native()},
                    ExplicitWin32Error{error});
            }
            const auto *const header =
                std::start_lifetime_as<Output>(output.data());
            auto current_vcn = header->StartingVcn.QuadPart;
            const auto extent_count = header->ExtentCount;
            const auto extents = std::span{
                std::start_lifetime_as_array<Extent>(
                    output.data() + offsetof(Output, Extents), extent_count),
                extent_count};
            for (const auto &extent : extents) {
                if (extent.Lcn.QuadPart >= 0) {
                    AddRange(
                        devicefs::terminal::FailFastCast<std::uint64_t>(
                            extent.Lcn.QuadPart),
                        devicefs::terminal::FailFastCast<std::uint64_t>(
                            extent.NextVcn.QuadPart - current_vcn));
                }
                current_vcn = extent.NextVcn.QuadPart;
            }
            if (completed) {
                return;
            }
            starting_vcn.QuadPart = current_vcn;
        }
    }

    auto ReadNamedDataStreams(
        const std::filesystem::path &path,
        const HANDLE object) -> void {
        auto streams = wistd::unique_ptr<FILE_STREAM_INFO>{};
        ThrowIfFileInfoFailed(
            wil::GetFileInfoNoThrow<FileStreamInfo>(object, streams),
            "failed to enumerate System Volume Information streams", path);
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
                "failed to enumerate System Volume Information", path);
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
    const auto cluster_size = [&options, volume, &device_path]
        -> std::uint64_t {
        if (options.cluster_size) {
            return *options.cluster_size;
        }
        const auto geometry = filesystem::QueryVolumeGeometry(volume);
        if (!geometry) {
            WinError("error occurred while attempting to enumerate SVI "
                "extents: failed to query the geometry for volume '{}'",
                std::wstring_view{device_path.native()},
                ExplicitWin32Error{geometry.error()});
        }
        const auto [_, cluster_size] = *geometry;
        return cluster_size;
    }();
    const auto root = device_path / L"System Volume Information";
    detail::SviExtentReader{cluster_size, add_range, options}.
        ReadTreeBlockOffsets(root);
}

// Return a bitmap of SVI extent data. A set bit indicates that at least some
// portion of the corresponding region contains SVI data.
//
// If the variant argument contains a `std::uint64_t`, it specifies the length
// of the device. If the variant argument contains a `BlockOffsetSet` object,
// that object is moved from, populated, and then returned.
//
// Precondition: the process has the backup reading privilege.
export [[nodiscard]] auto ReadBlockOffsets(
    const std::string_view snapshot_device,
    std::variant<std::monostate, std::uint64_t, devicefs::vss::BlockOffsetSet>
        extra_argument = {})
    -> devicefs::vss::BlockOffsetSet {
    const auto device_path = std::filesystem::path{snapshot_device};
    auto device = wil::unique_hfile{CreateFileW(
        device_path.c_str(), GENERIC_READ, detail::kShareMode, nullptr,
        OPEN_EXISTING, detail::kSecurityFlags, nullptr)};
    if (!device) {
        WinError("failed to open SVI snapshot volume '{}'",
            std::wstring_view{device_path.native()});
    }

    auto block_offsets = [&device, &device_path, &extra_argument] {
        if (const auto set = std::get_if<devicefs::vss::BlockOffsetSet>(
                &extra_argument)) {
            return std::move(*set);
        }
        const auto length = [&device, &device_path,
            &extra_argument = std::as_const(extra_argument)] {
            if (const auto volume_length = std::get_if<std::uint64_t>(
                    &extra_argument)) {
                return *volume_length;
            }
            const auto queried_length = filesystem::QueryDeviceLength(
                device.get());
            if (!queried_length) {
                WinError("failed to query the length of SVI volume '{}'",
                    std::wstring_view{device_path.native()},
                    ExplicitWin32Error{queried_length.error()});
            }
            return *queried_length;
        }();
        return devicefs::vss::BlockOffsetSet(length - 1);
    }();

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
