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

module devicefs.windows_block_device;

import std;
import devicefs.bitmap_set;
import devicefs.count_zeros;
import devicefs.terminal.safecast;
import devicefs.vss_block_descriptors;
import <devicefs/windows_imports.h>;

namespace devicefs {

using namespace std::string_view_literals;

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
    if (output.empty()) {
        return;
    }
    state_->bitmap.SynthesizeFreeClusters(
        output.data(), output.size(), offset);
}

auto LoadSnapshotAllocationBitmap(
    const std::string_view snapshot,
    const std::string_view description) -> SnapshotAllocationBitmap {
    auto device = WindowsBlockDevice<true>::FromFilename(
        std::filesystem::path{snapshot}, false, false, description);
    return SnapshotAllocationBitmap{std::make_unique<
        SnapshotAllocationBitmap::State>(
        SnapshotAllocationBitmap::State{
            .volume_size = device.length,
            .bitmap = std::move(device.allocation_bitmap),
        })};
}

template <std::uint64_t BlockSize>
auto ReadAllocationChangeBlocks(
    const std::string_view previous_snapshot,
    const std::string_view current_snapshot) -> AllocationChangeBlocks<BlockSize> {
    constexpr auto previous_description =
        "the previous snapshot"sv;
    constexpr auto current_description =
        "the current snapshot"sv;
    const auto previous = WindowsBlockDevice<true>::FromFilename(
        std::filesystem::path{previous_snapshot},
        false, false, previous_description);
    const auto current = WindowsBlockDevice<true>::FromFilename(
        std::filesystem::path{current_snapshot},
        false, false, current_description);
    const auto previous_size = previous.length;
    const auto current_size = current.length;
    if (previous_size != current_size) {
        throw std::runtime_error(std::format(
            "the previous snapshot '{}' is {} bytes, but the current snapshot "
            "'{}' is {} bytes",
            previous_snapshot, previous_size,
            current_snapshot, current_size));
    }
    const auto &previous_bitmap = previous.allocation_bitmap;
    const auto &current_bitmap = current.allocation_bitmap;
    if ((previous_bitmap.cluster_size != current_bitmap.cluster_size) ||
        (previous_bitmap.cluster_count != current_bitmap.cluster_count)) {
        throw std::runtime_error(std::format(
            "the previous snapshot '{}' has {} allocation clusters of {} bytes, "
            "but the current snapshot '{}' has {} clusters of {} bytes",
            previous_snapshot,
            previous_bitmap.cluster_count, previous_bitmap.cluster_size,
            current_snapshot,
            current_bitmap.cluster_count, current_bitmap.cluster_size));
    }

    const auto bitmap_size = previous_bitmap.BitmapByteCount();
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

    auto block_offsets = BitmapSet<BlockSize>(previous_size - 1);
    auto range_first = std::uint64_t{};
    auto range_limit = std::uint64_t{};
    const auto add_cluster = [&block_offsets, &range_first, &range_limit,
        cluster_size = previous_bitmap.cluster_size](const std::uint64_t cluster) {
        const auto start = cluster * cluster_size;
        const auto end = start + cluster_size;
        const auto first_block = start / BlockSize;
        const auto past_last_block = ((end - 1) / BlockSize) + 1;
        const auto first = first_block * BlockSize;
        const auto limit = past_last_block * BlockSize;
        // Clusters arrive in increasing order. Overlapping or adjacent block
        // ranges therefore need only one insertion, even across bitmap words.
        if (first > range_limit) {
            block_offsets.insert_range(range_first, range_limit);
            range_first = first;
        }
        range_limit = limit;
    };
    for (auto byte_index = 0uz; byte_index < bitmap_size;) {
        auto changed = filesystem_internal::LoadBitmapWord(
            previous_bitmap.BitmapData() + byte_index,
            bitmap_size - byte_index) ^
            filesystem_internal::LoadBitmapWord(
                current_bitmap.BitmapData() + byte_index,
                bitmap_size - byte_index);
        const auto next_byte = byte_index +
            std::min(sizeof(changed), bitmap_size - byte_index);
        if (changed == 0) {
            const auto remaining = previous_bits.subspan(next_byte);
            const auto mismatch = std::ranges::mismatch(
                remaining, current_bits.subspan(next_byte));
            byte_index = next_byte +
                devicefs::terminal::FailFastCast<std::size_t>(
                    std::ranges::distance(remaining.begin(), mismatch.in1));
            continue;
        }
        while (changed != 0) {
            const auto cluster =
                devicefs::terminal::FailFastMaybeCast<std::uint64_t>(
                    byte_index) * filesystem_internal::kBitsPerByte +
                CountTrailingZeros(changed);
            if (cluster >= previous_bitmap.cluster_count) {
                break;
            }
            add_cluster(cluster);
            changed &= changed - 1;
        }
        byte_index = next_byte;
    }
    block_offsets.insert_range(range_first, range_limit);
    return {
        .volume_size = previous_size,
        .block_offsets = std::move(block_offsets),
    };
}

template auto ReadAllocationChangeBlocks<vss::kBlockSize>(
    std::string_view, std::string_view)
    -> AllocationChangeBlocks<vss::kBlockSize>;

} // namespace devicefs
