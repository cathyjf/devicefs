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

#include <devicefs/strsafe_compat.h>

export module devicefs.svi_extents;

import std;
import <wil/filesystem.h>;
import <wil/safecast.h>;
import devicefs.common;
import devicefs.vss_block_descriptors;

namespace {

constexpr auto kShareMode =
    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

[[nodiscard]] auto OpenObject(const std::filesystem::path &path) {
    auto result = wil::unique_hfile{CreateFileW(
        path.c_str(), GENERIC_READ, kShareMode, nullptr, OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION |
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        nullptr)};
    if (!result) {
        WinError("could not open a System Volume Information object");
    }
    return result;
}

[[nodiscard]] auto QueryClusterSize(const HANDLE device) {
    auto ntfs = NTFS_VOLUME_DATA_BUFFER{};
    auto returned = DWORD{};
    if (!DeviceIoControl(device, FSCTL_GET_NTFS_VOLUME_DATA, nullptr, 0,
            &ntfs, sizeof(ntfs), &returned, nullptr)) {
        WinError("FSCTL_GET_NTFS_VOLUME_DATA failed");
    }
    [[gsl::suppress("26493",
        justification:
            "Braced initialization proves this construction safe at compile time.")]]
    return std::uint64_t{ntfs.BytesPerCluster};
}

auto ThrowIfFileInfoFailed(
    const HRESULT error,
    const std::string_view operation) -> void {
    if (SUCCEEDED(error)) {
        return;
    }
    WinError("{}", operation, ExplicitWin32Error::FromHresult(error));
}

class SviExtentReader {
  public:
    SviExtentReader(
        const std::uint64_t cluster_size,
        std::set<std::uint64_t> &block_offsets) noexcept
        : cluster_size_{cluster_size}, block_offsets_{block_offsets} {}

    auto ReadTreeBlockOffsets(const std::filesystem::path &root) -> void {
        auto root_handle = OpenObject(root);
        auto attributes = FILE_ATTRIBUTE_TAG_INFO{};
        ThrowIfFileInfoFailed(
            wil::GetFileInfoNoThrow<FileAttributeTagInfo>(
                root_handle.get(), &attributes),
            "could not query System Volume Information attributes");
        ReadObject(root, attributes.FileAttributes, root_handle.get());
    }

  private:
    auto AddRange(
        const std::uint64_t starting_cluster,
        const std::uint64_t cluster_count) -> void {
        const auto start = starting_cluster * cluster_size_;
        const auto end =
            start + (cluster_count * cluster_size_);
        const auto first_block = start / devicefs::vss::kBlockSize;
        const auto past_last_block =
            ((end - 1) / devicefs::vss::kBlockSize) + 1;
        block_offsets_.insert_range(
            std::views::iota(first_block, past_last_block) |
            std::views::transform([](const auto block) {
                return block * devicefs::vss::kBlockSize;
            }));
    }

    auto ReadExtents(const HANDLE object) -> void {
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
                    "could not retrieve System Volume Information extents",
                    ExplicitWin32Error{error});
            }
            const auto current_vcn = output.StartingVcn.QuadPart;
            const auto next_vcn = output.Extents[0].NextVcn.QuadPart;
            const auto lcn = output.Extents[0].Lcn.QuadPart;
            if (lcn >= 0) {
                AddRange(
                    wil::safe_cast_failfast<std::uint64_t>(lcn),
                    wil::safe_cast_failfast<std::uint64_t>(
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
            "could not enumerate System Volume Information streams");
        for (const auto &stream :
            wil::create_next_entry_offset_iterator(streams.get())) {
            const auto name = std::wstring_view{
                stream.StreamName,
                stream.StreamNameLength /
                    sizeof(std::wstring_view::value_type),
            };
            // ReadObject already measured the unnamed data stream through the
            // original object handle.
            if (name == L"::$DATA") {
                continue;
            }

            ReadExtents(OpenObject(
                std::format(L"{}{}", path.native(), name)).get());
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
                "could not enumerate System Volume Information");
            if (!entries) {
                return;
            }

            for (const auto &entry :
                wil::create_next_entry_offset_iterator(entries.get())) {
                const auto name = std::wstring_view{
                    entry.FileName,
                    entry.FileNameLength /
                        sizeof(std::wstring_view::value_type),
                };
                if ((name == L".") || (name == L"..")) {
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
        ReadExtents(object);
        ReadNamedDataStreams(path, object);
        if (((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) &&
            ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)) {
            EnumerateDirectory(path, object);
        }
    }

    const std::uint64_t cluster_size_;
    std::set<std::uint64_t> &block_offsets_;
};

} // namespace

export namespace devicefs::svi {

// The caller owns SeBackupPrivilege. Reparse-point objects and their named
// streams are measured, but directory reparse targets are not followed.
[[nodiscard]] auto ReadBlockOffsets(
    const std::wstring_view snapshot_device) -> std::set<std::uint64_t> {
    const auto device_path = std::filesystem::path{snapshot_device};
    auto device = wil::unique_hfile{CreateFileW(
        device_path.c_str(), GENERIC_READ, kShareMode, nullptr, OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr)};
    if (!device) {
        WinError("could not open the SVI snapshot volume");
    }

    const auto cluster_size = QueryClusterSize(device.get());
    const auto root = device_path / L"System Volume Information";
    auto block_offsets = std::set<std::uint64_t>{};
    SviExtentReader{cluster_size, block_offsets}.ReadTreeBlockOffsets(root);
    return block_offsets;
}

} // namespace devicefs::svi
