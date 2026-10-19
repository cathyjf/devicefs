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

export struct DirtyBlockMap {
    std::uint64_t volume_size;
    std::set<std::uint64_t> block_offsets;
    std::size_t descriptor_store_count;
    std::size_t descriptor_count;
    std::uint64_t descriptor_list_block_count;
    std::size_t descriptor_block_count;
    std::size_t allocation_block_count;
    std::size_t svi_block_count;
};

namespace {

using namespace std::string_view_literals;

[[nodiscard]] auto CollectDescriptorBlockOffsets(
    const std::span<const devicefs::vss::CatalogStoreDescriptors> stores) {
    auto result = std::set<std::uint64_t>{};
    for (const auto &store : stores) {
        const auto add = [&result, volume_size = store.blocks.volume_size]
            (const std::uint64_t offset) {
            if (offset < volume_size) {
                static_assert(std::has_single_bit(devicefs::vss::kBlockSize));
                result.insert(offset & ~(devicefs::vss::kBlockSize - 1));
            }
        };
        for (const auto &descriptor : store.blocks.descriptors) {
            add(descriptor.original_offset);
            add(descriptor.store_offset);
            if ((descriptor.flags & devicefs::vss::kForwarderFlag) != 0) {
                add(descriptor.relative_offset);
            }
        }
    }
    return result;
}

} // namespace

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
    const auto allocation_blocks = devicefs::ReadAllocationChangeBlocks(
        baseline_device, payload_device, devicefs::vss::kBlockSize);
    if (catalog.payload.volume_size != allocation_blocks.volume_size) {
        throw std::runtime_error(std::format(
            "the VSS descriptors for '{}' report a {}-byte volume, but the "
            "allocation bitmaps report {} bytes",
            payload_device,
            catalog.payload.volume_size, allocation_blocks.volume_size));
    }

    const auto svi_blocks = [baseline_device, payload_device] {
        auto privileges = ProcessPrivilegeEnabler{
            GetCurrentProcess(), std::array{wil::zwstring_view{SE_BACKUP_NAME}},
            "the backup privilege"sv};
        auto svi_blocks = devicefs::svi::ReadBlockOffsets(baseline_device);
        auto payload_svi_blocks = devicefs::svi::ReadBlockOffsets(payload_device);
        svi_blocks.merge(payload_svi_blocks);
        return svi_blocks;
    }();

    const auto stores = devicefs::vss::ReadBlockDescriptors(
        payload_device, catalog.stores);
    auto descriptor_blocks = CollectDescriptorBlockOffsets(stores);
    const auto descriptor_block_count = descriptor_blocks.size();
    descriptor_blocks.insert_range(allocation_blocks.block_offsets);
    descriptor_blocks.insert_range(svi_blocks);
    return DirtyBlockMap{
        .volume_size = catalog.payload.volume_size,
        .block_offsets = std::move(descriptor_blocks),
        .descriptor_store_count = stores.size(),
        .descriptor_count = std::ranges::fold_left(stores, std::size_t{},
            [](const auto count, const auto &store) {
                return count + store.blocks.descriptors.size();
            }),
        .descriptor_list_block_count = std::ranges::fold_left(stores, std::uint64_t{},
            [](const auto count, const auto &store) {
                return count + store.blocks.list_block_count;
            }),
        .descriptor_block_count = descriptor_block_count,
        .allocation_block_count = allocation_blocks.block_offsets.size(),
        .svi_block_count = svi_blocks.size(),
    };
}
