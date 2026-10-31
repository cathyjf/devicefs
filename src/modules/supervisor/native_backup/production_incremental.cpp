// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <devicefs/strsafe_compat.h>

module devicefs.supervisor.native_backup:production_incremental;

import std;
import <devicefs/common.h>;
import <devicefs/malloca.h>;
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

auto TryPrepareAndWriteDirtyMapForSnapshot(const auto &snapshot,
    const auto &baseline) -> std::optional<NamedFile> {
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
            return std::nullopt;
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
            return std::nullopt;
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
            return std::nullopt;
        }
        if (!SetHandleInformation(read_handle.get(),
                HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) {
            write_error(TryConstructWinError(
                "failed to make the read-only handle for '{}' "
                "inheritable", std::wstring_view{path.native()}));
            return std::nullopt;
        }
        devicefs::WriteToStream(devicefs::stdout,
            "Successfully prepared dirty map for volume '{}'.\n",
            snapshot.original_volume);
        // The reopened handle keeps the map readable after the writer
        // closes. Delete-on-close removes the file when the last handle
        // closes, after both the supervisor and DeviceFs release it.
        return NamedFile{
            .name = name + ".dirty.bitmap",
            .handle = std::move(read_handle),
        };
    } catch (const std::runtime_error &error) {
        write_error(error);
    }
    return std::nullopt;
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
        // `vshadow::Run` retains the set when its callback returns zero.
        // A return value of 1 instead makes it attempt to delete these
        // temporary snapshots after map preparation.
        constexpr auto kReturnValue = 1;

        // SVI enumeration requires the `SE_BACKUP_NAME` privilege. The
        // privilege helper is not currently reentrant, so we acquire the
        // privilege here before launching the threads.
        //
        // It is harmless that the individual threads will also attempt to
        // acquire the privilege.
        auto privilege_result =
            std::expected<void, std::unique_ptr<std::runtime_error>>{};
        auto privileges = ProcessPrivilegeEnabler{
            GetCurrentProcess(),
            std::array{wil::zwstring_view{SE_BACKUP_NAME}},
            "the backup privilege"sv, std::ref(privilege_result)};
        if (!privilege_result) {
            TryWriteNonFatalError(
                "unable to prepare dirty maps because of a failure while "
                "enabling process privileges", privilege_result.error());
            return kReturnValue;
        }

        using FutureType = std::future<std::optional<NamedFile>>;
        const auto length = std::ranges::distance(snapshots);
        const auto futures_buffer = NEW_MALLOCA_BUFFER(std::byte,
            sizeof(FutureType) * length);
        if (!futures_buffer) {
            throw std::bad_alloc{};
        }
        const auto futures_destroyer = [length](const FutureType *future) {
            std::destroy_n(future, length);
        };
        const auto futures = std::unique_ptr<FutureType[],
            decltype(futures_destroyer)>{
                ::new (futures_buffer.get()) FutureType[length],
                futures_destroyer};
        auto j = 0;
        for (auto i = snapshots.begin(); i != snapshots.end(); ++i) {
            // The first task will run on this thread once it is collected
            // below. If there is more than one task, each additional task
            // may run on a separate thread.
            const auto policy = std::launch::deferred |
                ((i != snapshots.begin()) ?
                    std::launch::async : std::launch{});
            const auto &snapshot = *i;
            futures[j++] = std::async(policy,
                [&snapshot, &baseline = baselines.at(
                    ParseVolumeGuid(snapshot.original_volume).value())] {
                return TryPrepareAndWriteDirtyMapForSnapshot(
                    snapshot, baseline);
            });
        }
        for (auto i = 0; i < length; ++i) {
            if (auto file = futures[i].get()) {
                files.push_back(std::move(*file));
            }
        }
        return kReturnValue;
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
