// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <devicefs/strsafe_compat.h>

module devicefs.supervisor.native_backup:production_incremental;

import std;
import <devicefs/common.h>;
import <devicefs/windows_imports.h>;
import devicefs.byte_comparison;
import devicefs.file_reader;
import devicefs.stream_writer;
import devicefs.supervisor.temporary_paths;
import devicefs.supervisor.vshadow;
import devicefs.terminal.safecast;
import devicefs.vss_block_descriptors;
import :incremental;
import :internal;
import :manifest;

namespace internal {

struct PreparedDirtyMaps {
    std::optional<std::int64_t> previous_backup_time;
    std::vector<NamedFile> files;
};

namespace {

auto MakeDirtyBitmap(const DirtyBlockMap &map) {
    constexpr auto bits_per_byte = std::numeric_limits<unsigned char>::digits;
    // `map.block_offsets` need not allocate storage for the whole volume.
    // The minimum bitmap length covers every volume chunk, and the reducer's
    // zero initialization marks the remaining in-volume chunks unchanged.
    const auto chunks = map.volume_size / kPbsImageChunkSize +
        ((map.volume_size % kPbsImageChunkSize) != 0);
    const auto bytes = devicefs::terminal::FailFastMaybeCast<std::size_t>(
        chunks / bits_per_byte + ((chunks % bits_per_byte) != 0));
    return devicefs::ReduceBitmap<const std::uint64_t,
        devicefs::vss::kBlockSize, kPbsImageChunkSize>(
        map.block_offsets.storage(), bytes);
}

} // namespace

auto PrepareDirtyMaps(const HANDLE cancellation_event,
    const devicefs::vshadow::SnapshotSet &payload,
    const std::optional<std::u8string> &namespace_override)
    -> PreparedDirtyMaps {
    const auto previous = [cancellation_event, &namespace_override]
        -> std::optional<PreviousBackupManifestResult::SnapshotManifest> {
        try {
            const auto result = RetrieveBackupManifest(
                cancellation_event, namespace_override, std::nullopt, true);
            if (result.exit_code != 0) {
                return std::nullopt;
            }
            return result.ParseManifest();
        } catch (const std::runtime_error &error) {
            TryWriteNonFatalError(
                "unable to prepare dirty maps because of a failure while "
                "retrieving or parsing the previous backup manifest", error);
            return std::nullopt;
        }
    }();
    if (!previous || !previous->backup_time) {
        return {};
    }
    // Creating `payload` enumerated existing snapshots as well as the new set.
    // The cache contains the properties collected by that enumeration, so the
    // manifest's snapshot IDs can be looked up without another VSS query.
    const auto baselines = previous->QuerySnapshotVolumes(true);
    auto snapshots = payload.snapshots | std::views::filter(
        [&baselines](const auto &snapshot) {
            return baselines.contains(
                ParseVolumeGuid(snapshot.original_volume).value());
        });
    if (snapshots.empty()) {
        return {};
    }
    const auto volumes = snapshots | std::views::transform(
        [](const auto &snapshot) static { return snapshot.original_volume; }) |
        std::ranges::to<std::vector>();
    auto files = std::vector<NamedFile>{};
    // VSS descriptor metadata is read through the payload snapshots. Creating
    // a newer snapshot on each volume makes that metadata readable. The maps
    // are built in the callback so the temporary snapshots still exist during
    // those reads.
    const auto build_maps = [&snapshots, &baselines, &files](
        const devicefs::vshadow::SnapshotSet &) {
        for (const auto &snapshot : snapshots) {
            const auto &baseline = baselines.at(
                ParseVolumeGuid(snapshot.original_volume).value());
            const auto name = SnapshotImageName(snapshot);
            const auto write_error = [&name](const auto &error) {
                TryWriteNonFatalError(std::format(
                    "failed to prepare the dirty map for '{}'", name), error);
            };
            try {
                const auto map = BuildDirtyBlockMap(
                    baseline.snapshot_identifier, snapshot.identifier,
                    baseline.device, snapshot.device);
                if (map.block_offsets.empty()) {
                    continue;
                }
                const auto path =
                    TemporarySystemDirectoryPath("devicefs-dirty-map");
                auto file = WriteEntireFile(path,
                    std::span<const unsigned char>{MakeDirtyBitmap(map)}, {
                        .overwrite = false,
                        .allow_readers = true,
                        .delete_on_close = true,
                    });
                if (!file) {
                    write_error(TryConstructWinError(
                        "failed to create or write file '{}'",
                        std::wstring_view{path.native()}, file.error()));
                    continue;
                }
                // DeviceFs receives a separate read-only handle so it cannot
                // change the map. The sharing flags allow the original handle's
                // write access and delete-on-close setting; otherwise reopening
                // the file would fail.
                //
                // Overlapped I/O allows separate reads through this handle to
                // run concurrently.
                auto read_handle = wil::unique_hfile{ReOpenFile(file->get(),
                    GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    FILE_FLAG_OVERLAPPED)};
                if (!read_handle) {
                    write_error(TryConstructWinError(
                        "failed to reopen dirty-map file '{}' read-only",
                        std::wstring_view{path.native()}));
                    continue;
                }
                if (!SetHandleInformation(read_handle.get(),
                        HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
                    write_error(TryConstructWinError(
                        "failed to make the read-only handle for '{}' "
                        "inheritable", std::wstring_view{path.native()}));
                    continue;
                }
                // The reopened handle keeps the map readable after the writer
                // closes. Delete-on-close removes the file when the last handle
                // closes, after both the supervisor and DeviceFs release it.
                files.push_back({
                    .name = name + ".dirty.bitmap",
                    .handle = std::move(read_handle),
                });
                devicefs::WriteToStream(devicefs::stdout,
                    "Successfully prepared dirty map for volume '{}'.\n",
                    snapshot.original_volume);
            } catch (const std::runtime_error &error) {
                write_error(error);
            }
        }
        // `vshadow::Run` retains the set when its callback returns zero.
        // Returning one instead makes it attempt to delete these temporary
        // snapshots after map preparation.
        return 1;
    };
    try {
        std::ignore = RunVssOperation(cancellation_event,
            [cancellation_event, &volumes, &build_maps] {
                return devicefs::vshadow::Run(
                    cancellation_event, false, volumes, build_maps);
            });
    } catch (const std::runtime_error &error) {
        TryWriteNonFatalError(
            "failed to prepare dirty maps because creating the temporary "
            "successor snapshot set failed", error);
        return {};
    }
    return {
        .previous_backup_time = files.empty()
            ? std::nullopt : previous->backup_time,
        .files = std::move(files),
    };
}

} // namespace internal
