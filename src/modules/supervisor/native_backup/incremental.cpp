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

export module devicefs.supervisor.native_backup:incremental;

import std;
import <devicefs/windows_imports.h>;
import devicefs.supervisor.process_privileges;
import devicefs.filesystem;
import devicefs.svi_extents;
import devicefs.vss_block_descriptors;
import devicefs.stream_writer;

extern template auto devicefs::ReadAllocationChangeBlocks<
    devicefs::vss::kBlockSize>(std::string_view, std::string_view)
    -> devicefs::AllocationChangeBlocks<devicefs::vss::kBlockSize>;

export struct DirtyBlockMap {
    std::uint64_t volume_size;
    devicefs::vss::BlockOffsetSet block_offsets;
    std::size_t descriptor_store_count;
    std::size_t descriptor_count;
    std::uint64_t descriptor_list_block_count;
    std::size_t descriptor_block_count;
    std::size_t allocation_block_count;
    std::size_t svi_block_count;
};

using namespace std::string_view_literals;

// Build the candidate dirty-block map using the snapshot volume devices named
// by the paths `baseline_device` and `payload_device`.
// The result contains 16-KiB block-start byte offsets selected by descriptors
// from every contributing store, allocation changes between the snapshots,
// and SVI extents in both snapshots. It also reports descriptor and list-block
// totals and each contribution's number of unique block offsets before the
// final union. The function throws if a required input cannot be read or
// validated.
export [[nodiscard]] auto BuildDirtyBlockMap(
    const GUID &baseline_snapshot_identifier,
    const GUID &payload_snapshot_identifier,
    const std::string_view baseline_device,
    const std::string_view payload_device)
    -> DirtyBlockMap {
    const auto catalog = devicefs::vss::ReadBlockDescriptorIntervalCatalog(
        payload_device, baseline_snapshot_identifier, payload_snapshot_identifier);
    devicefs::WriteToStream(devicefs::stdout,
        "Building the dirty map for an interval containing {} "
        "snapshot(s), including A and B.\n",
        catalog.stores.size() + 1);
    auto allocation_blocks = devicefs::ReadAllocationChangeBlocks<
        devicefs::vss::kBlockSize>(
        baseline_device, payload_device);
    const auto allocation_block_count = allocation_blocks.block_offsets.size();
    if (catalog.payload.volume_size != allocation_blocks.volume_size) {
        throw std::runtime_error(std::format(
            "the VSS descriptors for '{}' report a {}-byte volume, but the "
            "allocation bitmaps report {} bytes",
            payload_device,
            catalog.payload.volume_size, allocation_blocks.volume_size));
    }

    auto svi_blocks = [baseline_device, payload_device] {
        auto privileges = ProcessPrivilegeEnabler{
            GetCurrentProcess(), std::array{wil::zwstring_view{SE_BACKUP_NAME}},
            "the backup privilege"sv};
        auto svi_blocks = devicefs::svi::ReadBlockOffsets(baseline_device);
        auto payload_svi_blocks = devicefs::svi::ReadBlockOffsets(payload_device);
        svi_blocks.merge(payload_svi_blocks);
        return svi_blocks;
    }();
    const auto svi_block_count = svi_blocks.size();

    auto descriptor_blocks = devicefs::vss::BlockOffsetSet(
        catalog.payload.volume_size - 1);
    const auto counts = devicefs::vss::EnumerateBlockDescriptors(
        payload_device, catalog.stores,
        [&descriptor_blocks, volume_size = catalog.payload.volume_size](
            const auto &descriptor) {
            const auto add = [&descriptor_blocks, volume_size](
                const std::uint64_t offset) {
                if (offset < volume_size) {
                    static_assert(std::has_single_bit(devicefs::vss::kBlockSize));
                    descriptor_blocks.insert(
                        offset & ~(devicefs::vss::kBlockSize - 1));
                }
            };
            add(descriptor.original_offset);
            add(descriptor.store_offset);
            if ((descriptor.flags & devicefs::vss::kForwarderFlag) != 0) {
                add(descriptor.relative_offset);
            }
        });
    const auto descriptor_block_count = descriptor_blocks.size();
    descriptor_blocks.merge(allocation_blocks.block_offsets);
    descriptor_blocks.merge(svi_blocks);
    return DirtyBlockMap{
        .volume_size = catalog.payload.volume_size,
        .block_offsets = std::move(descriptor_blocks),
        .descriptor_store_count = catalog.stores.size(),
        .descriptor_count = counts.descriptor_count,
        .descriptor_list_block_count = counts.list_block_count,
        .descriptor_block_count = descriptor_block_count,
        .allocation_block_count = allocation_block_count,
        .svi_block_count = svi_block_count,
    };
}
