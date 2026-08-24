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
import :internal;
import :manifest;
import :privileges;
import devicefs.filesystem;
import devicefs.stream_writer;
import devicefs.supervisor.vshadow;
import devicefs.svi_extents;
import devicefs.vss_block_descriptors;

namespace {

struct DirtyBlockMap {
    std::uint64_t volume_size;
    std::vector<std::uint64_t> block_offsets;
    std::size_t descriptor_count;
    std::uint64_t descriptor_list_block_count;
    std::size_t descriptor_block_count;
    std::size_t allocation_block_count;
    std::size_t svi_block_count;
};

struct VolumeReport {
    GUID volume_identifier;
    std::expected<DirtyBlockMap, std::string> map;
};

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

[[nodiscard]] auto BuildDirtyBlockMap(
    const GUID &baseline_snapshot_identifier,
    const std::wstring_view baseline_device,
    const std::wstring_view payload_device) {
    const auto descriptors = devicefs::vss::ReadBlockDescriptors(
        payload_device, baseline_snapshot_identifier);
    auto descriptor_blocks = ProjectDescriptorBlocks(descriptors);
    auto allocation_blocks = devicefs::ReadAllocationChangeBlocks(
        baseline_device, payload_device, devicefs::vss::kBlockSize);
    if (descriptors.volume_size != allocation_blocks.volume_size) {
        throw std::runtime_error(
            "the descriptor and allocation-bitmap volume sizes do not match");
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

[[nodiscard]] auto VolumeName(const GUID &identifier) {
    const auto text = winrt::to_hstring(identifier);
    return std::format(
        L"\\\\?\\Volume{}\\", std::wstring_view{text});
}

[[nodiscard]] constexpr auto VolumeBlockCount(const std::uint64_t volume_size) {
    return volume_size / devicefs::vss::kBlockSize +
        ((volume_size % devicefs::vss::kBlockSize) != 0);
}

auto PrintStatistics(const std::span<const VolumeReport> reports) {
    auto &output = devicefs::WriteToStream(
        std::cout, "Incremental dirty-block statistics:\n");
    auto mapped = std::size_t{};
    auto candidate_total = std::size_t{};
    auto volume_block_total = std::uint64_t{};
    for (const auto &report : reports) {
        const auto volume_identifier =
            winrt::to_hstring(report.volume_identifier);
        devicefs::WriteToStream(output, L"\n  Volume ID: {}\n",
            std::wstring_view{volume_identifier});
        if (!report.map) {
            devicefs::WriteToStream(output,
                "    Dirty map unavailable: {}\n", report.map.error());
            continue;
        }

        ++mapped;
        const auto &map = *report.map;
        const auto volume_blocks = VolumeBlockCount(map.volume_size);
        const auto candidate_blocks = map.block_offsets.size();
        const auto percentage =
            (static_cast<double>(candidate_blocks) * 100.0) /
            static_cast<double>(volume_blocks);
        candidate_total += candidate_blocks;
        volume_block_total += volume_blocks;
        devicefs::WriteToStream(output,
            "    Volume size: {} bytes\n"
            "    Potentially dirty: {} of {} 16-KiB blocks ({:.2f}%)\n"
            "    Descriptor evidence: {} unique block(s) from {} "
            "descriptor(s) in {} list block(s)\n"
            "    Allocation changes: {} unique block(s)\n"
            "    System Volume Information extents: {} unique block(s)\n",
            map.volume_size,
            candidate_blocks, volume_blocks, percentage,
            map.descriptor_block_count, map.descriptor_count,
            map.descriptor_list_block_count,
            map.allocation_block_count,
            map.svi_block_count);
    }

    const auto unavailable = reports.size() - mapped;
    const auto percentage = volume_block_total == 0
        ? 0.0
        : (static_cast<double>(candidate_total) * 100.0) /
            static_cast<double>(volume_block_total);
    devicefs::WriteToStream(output,
        "\nSummary: {} volume(s) mapped, {} unavailable; {} of {} "
        "16-KiB blocks potentially dirty ({:.2f}%).\n",
        mapped, unavailable, candidate_total, volume_block_total, percentage);
    if (!output) {
        throw std::runtime_error(
            "could not write the incremental dirty-block statistics");
    }
}

} // namespace

export [[nodiscard]] auto RunIncrementalStats(
    const HANDLE cancellation_event,
    const std::optional<std::u8string> &namespace_override) -> int {
    const auto previous = RetrievePreviousBackupManifest(
        cancellation_event, namespace_override);
    if (!previous) {
        return internal::kCancelledExitCode;
    }
    if (previous->exit_code != 0) {
        return previous->exit_code;
    }

    const auto snapshot_volumes =
        previous->ParseManifest().QuerySnapshotVolumes();
    if (snapshot_volumes.empty()) {
        throw std::runtime_error(
            "the previous backup has no snapshots still available");
    }
    if (internal::CancellationRequested(cancellation_event)) {
        return internal::kCancelledExitCode;
    }

    const auto volumes = snapshot_volumes |
        std::views::keys |
        std::views::transform(VolumeName) |
        std::ranges::to<std::vector<std::wstring>>();
    auto reports = std::vector<VolumeReport>{};

    // A zero callback result retains the snapshot set. This private result
    // makes the existing nested Run owners clean up C and then B, and is
    // translated back to a successful statistics result below.
    constexpr auto kTemporarySnapshotsComplete = 1;
    const auto snapshot_result = internal::RunVssOperation(
        cancellation_event,
        [&] {
            return devicefs::vshadow::Run(
                cancellation_event, false, volumes,
                [&](const devicefs::vshadow::SnapshotSet &payload) {
                    if (internal::CancellationRequested(
                            cancellation_event)) {
                        return internal::kCancelledExitCode;
                    }
                    // C only makes payload B nonlatest; it is not a map input.
                    return devicefs::vshadow::Run(
                        cancellation_event, false, volumes,
                        [&](const devicefs::vshadow::SnapshotSet &) {
                            for (auto &&[baseline, current] :
                                std::views::zip(
                                    snapshot_volumes,
                                    payload.snapshots)) {
                                if (internal::CancellationRequested(
                                        cancellation_event)) {
                                    return internal::kCancelledExitCode;
                                }
                                const auto &[volume_identifier,
                                    baseline_snapshot] = baseline;
                                try {
                                    reports.push_back({
                                        .volume_identifier =
                                            volume_identifier,
                                        .map = BuildDirtyBlockMap(
                                            baseline_snapshot.snapshot_identifier,
                                            baseline_snapshot.device,
                                            current.device),
                                    });
                                } catch (const std::runtime_error &error) {
                                    reports.push_back({
                                        .volume_identifier =
                                            volume_identifier,
                                        .map = std::unexpected{
                                            std::string{error.what()}},
                                    });
                                }
                            }
                            return kTemporarySnapshotsComplete;
                        });
                });
        });
    if (snapshot_result != kTemporarySnapshotsComplete) {
        return snapshot_result;
    }
    if (internal::CancellationRequested(cancellation_event)) {
        return internal::kCancelledExitCode;
    }
    PrintStatistics(reports);
    return 0;
}
