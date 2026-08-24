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

#include <wil/resource.h>
#include <wil/safecast.h>

module devicefs.filesystem;

import std;
import devicefs.common;

namespace {

[[nodiscard]] auto OpenSnapshot(
    const std::wstring_view source,
    const std::string_view description) {
    const auto path = std::filesystem::path{source};
    auto result = wil::unique_hfile{CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED |
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
        nullptr)};
    if (!result) {
        WinError("could not open {}", description);
    }
    return result;
}

[[nodiscard]] auto QueryVolumeSize(
    const HANDLE volume,
    const std::string_view description) {
    auto length = GET_LENGTH_INFORMATION{};
    const auto error = devicefs::filesystem_internal::Ioctl(
        volume, IOCTL_DISK_GET_LENGTH_INFO, &length, sizeof(length));
    if (error != ERROR_SUCCESS) {
        WinError("IOCTL_DISK_GET_LENGTH_INFO failed for {}", description,
            ExplicitWin32Error{error});
    }
    return wil::safe_cast_failfast<std::uint64_t>(
        length.Length.QuadPart);
}

} // namespace

namespace devicefs {

struct SnapshotAllocationBitmap::State {
    std::uint64_t volume_size;
    filesystem_internal::AllocationBitmap bitmap;
};

SnapshotAllocationBitmap::SnapshotAllocationBitmap(
    std::unique_ptr<State> state) noexcept
    : state_{std::move(state)} {}

SnapshotAllocationBitmap::SnapshotAllocationBitmap(
    SnapshotAllocationBitmap &&) noexcept = default;

auto SnapshotAllocationBitmap::operator=(
    SnapshotAllocationBitmap &&) noexcept
    -> SnapshotAllocationBitmap & = default;

SnapshotAllocationBitmap::~SnapshotAllocationBitmap() = default;

auto SnapshotAllocationBitmap::VolumeSize() const noexcept
    -> std::uint64_t {
    return state_->volume_size;
}

auto SnapshotAllocationBitmap::ClusterSize() const noexcept
    -> std::uint32_t {
    return state_->bitmap.cluster_size;
}

auto SnapshotAllocationBitmap::SynthesizeFreeClusters(
    const std::span<unsigned char> output,
    const std::uint64_t offset) const noexcept -> void {
    state_->bitmap.SynthesizeFreeClusters(output, offset);
}

auto LoadSnapshotAllocationBitmap(
    const std::wstring_view snapshot,
    const std::string_view description) -> SnapshotAllocationBitmap {
    auto handle = OpenSnapshot(snapshot, description);
    const auto size = QueryVolumeSize(handle.get(), description);
    return SnapshotAllocationBitmap{std::make_unique<
        SnapshotAllocationBitmap::State>(
        SnapshotAllocationBitmap::State{
            .volume_size = size,
            .bitmap = filesystem_internal::LoadAllocationBitmap(
                handle.get(), size, description),
        })};
}

auto ReadAllocationChangeBlocks(
    const std::wstring_view previous_snapshot,
    const std::wstring_view current_snapshot,
    const std::uint64_t block_size) -> AllocationChangeBlocks {
    constexpr auto previous_description =
        std::string_view{"the previous snapshot"};
    constexpr auto current_description =
        std::string_view{"the current snapshot"};
    auto previous = OpenSnapshot(
        previous_snapshot, previous_description);
    auto current = OpenSnapshot(
        current_snapshot, current_description);
    const auto previous_size =
        QueryVolumeSize(previous.get(), previous_description);
    const auto current_size =
        QueryVolumeSize(current.get(), current_description);
    if (previous_size != current_size) {
        throw std::runtime_error(
            "the previous and current snapshot volume sizes do not match");
    }
    auto previous_bitmap = filesystem_internal::LoadAllocationBitmap(
        previous.get(), previous_size, previous_description);
    auto current_bitmap = filesystem_internal::LoadAllocationBitmap(
        current.get(), current_size, current_description);
    if ((previous_bitmap.cluster_size != current_bitmap.cluster_size) ||
        (previous_bitmap.cluster_count != current_bitmap.cluster_count)) {
        throw std::runtime_error(
            "the previous and current snapshot allocation geometry does not "
            "match");
    }

    const auto bitmap_byte_count =
        previous_bitmap.cluster_count / filesystem_internal::kBitsPerByte +
        ((previous_bitmap.cluster_count %
            filesystem_internal::kBitsPerByte) != 0);
    // LoadAllocationBitmap bounds its complete output by DWORD, so the bitmap
    // byte count is representable by size_t on every Windows target.
    static_assert(std::numeric_limits<DWORD>::max() <=
        std::numeric_limits<std::size_t>::max());
    const auto bitmap_size =
        wil::safe_cast_failfast<std::size_t>(bitmap_byte_count);
    const auto previous_bits = std::as_bytes(std::span{
        previous_bitmap.storage.get() +
            filesystem_internal::kVolumeBitmapHeaderSize,
        bitmap_size,
    });
    const auto current_bits = std::as_bytes(std::span{
        current_bitmap.storage.get() +
            filesystem_internal::kVolumeBitmapHeaderSize,
        bitmap_size,
    });

    auto block_offsets = std::vector<std::uint64_t>{};
    const auto add_cluster = [&](const std::uint64_t cluster) {
        const auto start = cluster * previous_bitmap.cluster_size;
        const auto end = start + previous_bitmap.cluster_size;
        const auto first_block = start / block_size;
        const auto past_last_block = ((end - 1) / block_size) + 1;
        for (const auto block :
            std::views::iota(first_block, past_last_block)) {
            const auto offset = block * block_size;
            if (block_offsets.empty() ||
                (block_offsets.back() != offset)) {
                block_offsets.push_back(offset);
            }
        }
    };
    for (auto byte_index = 0uz; byte_index < bitmap_size; ++byte_index) {
        auto changed = std::to_integer<unsigned int>(
            previous_bits[byte_index] ^ current_bits[byte_index]);
        while (changed != 0) {
            const auto cluster =
                wil::safe_cast_failfast<std::uint64_t>(byte_index) *
                    filesystem_internal::kBitsPerByte +
                wil::safe_cast_failfast<std::uint64_t>(
                    std::countr_zero(changed));
            if (cluster >= previous_bitmap.cluster_count) {
                break;
            }
            add_cluster(cluster);
            changed &= changed - 1;
        }
    }
    return {
        .volume_size = previous_size,
        .block_offsets = std::move(block_offsets),
    };
}

} // namespace devicefs
