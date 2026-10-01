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
import devicefs.supervisor.vshadow;
import devicefs.guid_formatter;
import <devicefs/common.h>;

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
using namespace std::chrono_literals;

// A changed catalog is retried a fixed number of times. A short pause between
// attempts allows a pending cleanup operation to finish before trying again.
constexpr auto kSnapshotAcquisitionAttempts = 3;
constexpr auto kSnapshotAcquisitionRetryDelay = 100ms;

using SnapshotHandles = std::vector<std::pair<GUID, wil::unique_hfile>>;

struct RetainedSnapshotInterval {
    devicefs::vss::CatalogInterval catalog;
    SnapshotHandles handles;
};

[[nodiscard]] auto TryAcquireSnapshotInterval(
    const std::string_view payload_device,
    const GUID &baseline_identifier,
    const GUID &payload_identifier,
    SnapshotHandles &handles)
    -> std::expected<devicefs::vss::CatalogInterval, std::runtime_error> {
    auto catalog = devicefs::vss::ReadBlockDescriptorIntervalCatalog(
        payload_device, baseline_identifier, payload_identifier);
    const auto pending_identifiers = std::array{
        std::span<const devicefs::vss::CatalogStore>{catalog.stores},
        std::span<const devicefs::vss::CatalogStore>{std::array{catalog.payload}}
    } | std::views::join | std::views::transform(
        [](const auto &store) { return *store.snapshot_identifier; }) |
        std::views::filter([&handles](const GUID &identifier) {
            return std::ranges::none_of(handles, [&identifier](const auto &snapshot) {
                return InlineIsEqualGUID(snapshot.first, identifier);
            });
        }) |
        std::ranges::to<std::vector>();
    const auto properties = devicefs::vshadow::QuerySnapshotProperties(pending_identifiers);
    handles.append_range(std::views::zip_transform([payload_device](
        const GUID &identifier, const auto &snapshot) {
        if (!snapshot) {
            throw std::runtime_error(std::format(
                "could not query VSS snapshot '{}' while retaining the "
                "descriptor interval in '{}'", FormatGuid(identifier), payload_device));
        }
        auto handle = wil::unique_hfile{CreateFileA(
            snapshot->device.c_str(), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr)};
        if (!handle) {
            WinError(
                "could not open VSS snapshot '{}' at '{}' to retain "
                "the descriptor interval in '{}'",
                [identifier] {
                    auto last_error = wil::last_error_context{};
                    return FormatGuid(identifier);
                }(),
                snapshot->device, payload_device);
        }
        return std::pair{identifier, std::move(handle)};
    }, pending_identifiers, properties));
    // Snapshot cleanup can change the catalog between its first read and
    // handle acquisition. A second read with all handles open checks that
    // the selected stores and their metadata locations have stayed unchanged.
    const auto acquired_catalog = devicefs::vss::ReadBlockDescriptorIntervalCatalog(
        payload_device, baseline_identifier, payload_identifier);
    if (catalog != acquired_catalog) {
        return std::unexpected{std::runtime_error(std::format(
            "VSS descriptor interval '{}' to '{}' in '{}' changed "
            "while its snapshot handles were being opened",
            FormatGuid(baseline_identifier), FormatGuid(payload_identifier), payload_device))};
    }
    return catalog;
}

[[nodiscard]] auto AcquireSnapshotIntervalWithRetries(
    const std::string_view payload_device,
    const GUID &baseline_identifier,
    const GUID &payload_identifier)
    -> RetainedSnapshotInterval {
    // Handles acquired in one attempt stay open through subsequent attempts
    // and their pauses. Releasing them would let snapshot cleanup undo the
    // progress made by earlier acquisitions.
    auto handles = SnapshotHandles{};
    for (auto attempt = 1; ; ++attempt) {
        auto acquired = TryAcquireSnapshotInterval(
            payload_device, baseline_identifier, payload_identifier, handles);
        if (acquired) {
            return RetainedSnapshotInterval{
                .catalog = std::move(*acquired), .handles = std::move(handles)};
        }
        if (attempt == kSnapshotAcquisitionAttempts) {
            throw acquired.error();
        }
        std::this_thread::sleep_for(kSnapshotAcquisitionRetryDelay);
    }
}

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
    // Descriptor, allocation, and SVI reads are separate operations. The
    // snapshot handles cover that entire sequence and close when map
    // construction returns, rather than after catalog discovery alone.
    const auto retained = AcquireSnapshotIntervalWithRetries(
        payload_device, baseline_snapshot_identifier, payload_snapshot_identifier);
    const auto allocation_blocks = devicefs::ReadAllocationChangeBlocks(
        baseline_device, payload_device, devicefs::vss::kBlockSize);
    if (retained.catalog.payload.volume_size != allocation_blocks.volume_size) {
        throw std::runtime_error(std::format(
            "the VSS descriptors for '{}' report a {}-byte volume, but the "
            "allocation bitmaps report {} bytes",
            payload_device,
            retained.catalog.payload.volume_size, allocation_blocks.volume_size));
    }

    const auto svi_blocks = [baseline_device, payload_device] {
        auto privileges = ProcessPrivilegeEnabler{
            GetCurrentProcess(), std::array{wil::zwstring_view{SE_BACKUP_NAME}},
            "the backup privilege"sv};
        auto svi_blocks = devicefs::svi::ReadBlockOffsets(baseline_device);
        auto payload_svi_blocks = devicefs::svi::ReadBlockOffsets(payload_device);
        svi_blocks.merge(payload_svi_blocks);
        privileges.Restore();
        return svi_blocks;
    }();

    const auto stores = devicefs::vss::ReadBlockDescriptors(
        payload_device, retained.catalog.stores);
    auto descriptor_blocks = CollectDescriptorBlockOffsets(stores);
    const auto descriptor_block_count = descriptor_blocks.size();
    descriptor_blocks.insert_range(allocation_blocks.block_offsets);
    descriptor_blocks.insert_range(svi_blocks);
    return DirtyBlockMap{
        .volume_size = retained.catalog.payload.volume_size,
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
