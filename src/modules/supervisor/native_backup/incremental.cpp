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
import :privileges;
import devicefs.filesystem;
import devicefs.svi_extents;
import devicefs.vss_block_descriptors;

export struct DirtyBlockMap {
    std::uint64_t volume_size;
    std::vector<std::uint64_t> block_offsets;
    std::size_t descriptor_count;
    std::uint64_t descriptor_list_block_count;
    std::size_t descriptor_block_count;
    std::size_t allocation_block_count;
    std::size_t svi_block_count;
};

namespace {

auto SortAndDeduplicate(
    std::vector<std::uint64_t> &offsets) -> void {
    std::ranges::sort(offsets);
    const auto duplicate_tail = std::ranges::unique(offsets);
    offsets.erase(duplicate_tail.begin(), duplicate_tail.end());
}

[[nodiscard]] auto ProjectDescriptorBlocks(
    const devicefs::vss::StoreBlockDescriptors &source) {
    auto result = std::vector<std::uint64_t>{};
    const auto add = [&](const std::uint64_t offset) {
        if (offset < source.volume_size) {
            result.push_back(
                offset - (offset % devicefs::vss::kBlockSize));
        }
    };
    for (const auto &descriptor : source.descriptors) {
        add(descriptor.original_offset);
        add(descriptor.store_offset);
        if ((descriptor.flags & devicefs::vss::kForwarderFlag) != 0) {
            add(descriptor.relative_offset);
        }
    }
    SortAndDeduplicate(result);
    return result;
}

} // namespace

export [[nodiscard]] auto BuildDirtyBlockMap(
    const GUID &baseline_snapshot_identifier,
    const std::string_view baseline_device,
    const std::string_view payload_device) {
    const auto descriptors = devicefs::vss::ReadBlockDescriptors(
        payload_device, baseline_snapshot_identifier);
    auto descriptor_blocks = ProjectDescriptorBlocks(descriptors);
    auto allocation_blocks = devicefs::ReadAllocationChangeBlocks(
        baseline_device, payload_device, devicefs::vss::kBlockSize);
    if (descriptors.volume_size != allocation_blocks.volume_size) {
        throw std::runtime_error(std::format(
            "the VSS descriptors for '{}' report a {}-byte volume, but the "
            "allocation bitmaps report {} bytes",
            payload_device,
            descriptors.volume_size, allocation_blocks.volume_size));
    }

    constexpr auto privilege_names =
        std::array{wil::zwstring_view{SE_BACKUP_NAME}};
    constexpr auto privilege_description =
        std::string_view{"the backup privilege"};
    auto privileges = internal::ProcessPrivilegeEnabler{
        GetCurrentProcess(), privilege_names,
        privilege_description};
    auto svi_blocks = devicefs::svi::ReadBlockOffsets(baseline_device);
    auto payload_svi_blocks =
        devicefs::svi::ReadBlockOffsets(payload_device);
    svi_blocks.merge(payload_svi_blocks);
    privileges.Restore();

    const auto descriptor_block_count = descriptor_blocks.size();
    const auto allocation_block_count =
        allocation_blocks.block_offsets.size();
    const auto svi_block_count = svi_blocks.size();
    descriptor_blocks.append_range(allocation_blocks.block_offsets);
    descriptor_blocks.append_range(svi_blocks);
    SortAndDeduplicate(descriptor_blocks);
    return DirtyBlockMap{
        .volume_size = descriptors.volume_size,
        .block_offsets = std::move(descriptor_blocks),
        .descriptor_count = descriptors.descriptors.size(),
        .descriptor_list_block_count = descriptors.list_block_count,
        .descriptor_block_count = descriptor_block_count,
        .allocation_block_count = allocation_block_count,
        .svi_block_count = svi_block_count,
    };
}
