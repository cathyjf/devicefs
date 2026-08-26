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

#include <cstddef>

export module devicefs.supervisor.native_backup:incremental_diagnostics;

import std;
import <devicefs/windows_imports.h>;
import :devicefs_process;
import :filesystem_verifier;
import :incremental;
import :internal;
import :manifest;
import devicefs.common;
import devicefs.filesystem;
import devicefs.rpc_block_device_server;
import devicefs.stream_writer;
import devicefs.synthetic_backup_block_device;
import devicefs.supervisor.vshadow;
import devicefs.vss_block_descriptors;

export struct IncrementalDiagnosticOptions {
    bool print_statistics = false;
    bool verify = false;
    std::optional<std::wstring> synthetic_backup_prefix;
    std::optional<std::wstring> filesystem_verification_prefix;
    double filesystem_verification_percentage = 100.0;
    std::optional<GUID> baseline_snapshot_identifier;
    std::optional<std::u8string> namespace_override;
    std::vector<std::wstring> volume_override;
};

namespace {

using namespace std::chrono_literals;

// Each volume owns its worker pool, so work assigned to another volume cannot
// consume its workers. The two 4-MiB buffers per worker bound storage to
// 32 MiB per volume while allowing four independent reads to remain in flight.
constexpr auto kComparisonChunkSize = 4uz * 1024 * 1024;
constexpr auto kVerificationWorkerCount = 4uz;
constexpr auto kProgressReportInterval = 1min;
// Polling only schedules progress and observes completion; 100 ms avoids a
// noticeable final-report delay without busy-waiting.
constexpr auto kWorkerPollInterval = 100ms;
constexpr auto kBaselineDescription =
    std::string_view{"the retained snapshot"};
constexpr auto kPayloadDescription =
    std::string_view{"the new snapshot"};
static_assert(devicefs::vss::kBlockSize <=
    std::numeric_limits<std::size_t>::max());
constexpr auto kMapBlockSize =
    std::size_t{devicefs::vss::kBlockSize};
static_assert((kComparisonChunkSize % kMapBlockSize) == 0);
static_assert(kComparisonChunkSize <=
    std::numeric_limits<DWORD>::max());

// A zero callback result retains a snapshot set. Diagnostic B and C are
// temporary, so both nested VSS callbacks return this private nonzero result.
constexpr auto kTemporarySnapshotsComplete = 1;

struct AvailableBaseline {
    GUID volume_identifier;
    GUID snapshot_identifier;
    std::wstring volume;
    std::wstring device;
};

struct SnapshotInterval {
    GUID volume_identifier;
    GUID baseline_snapshot_identifier;
    GUID payload_snapshot_identifier;
    std::wstring volume;
    std::wstring baseline_device;
    std::wstring payload_device;
};

struct VolumeReport {
    GUID volume_identifier;
    GUID baseline_snapshot_identifier;
    GUID payload_snapshot_identifier;
    std::wstring volume;
    std::wstring baseline_device;
    std::wstring payload_device;
    std::expected<DirtyBlockMap, std::string> map;
};

struct DirtyBlockReportResult {
    int exit_code;
    std::vector<VolumeReport> reports;
};

struct IncrementalDiagnosticResult {
    int map_exit_code = 0;
    int verification_exit_code = 0;
    int filesystem_verification_exit_code = 0;
    std::vector<VolumeReport> reports;
};

struct SnapshotDiagnosticResult {
    int exit_code;
    IncrementalDiagnosticResult diagnostics;
};

[[nodiscard]] auto VolumeName(const GUID &identifier) {
    const auto text = winrt::to_hstring(identifier);
    return std::format(
        L"\\\\?\\Volume{}\\", std::wstring_view{text});
}

[[nodiscard]] auto CollectAvailableBaselines(
    const PreviousBackupManifestResult::SnapshotManifest::SnapshotVolumes
        &snapshot_volumes) {
    return snapshot_volumes |
        std::views::transform([](const auto &entry) {
            const auto &[volume_identifier, snapshot] = entry;
            return AvailableBaseline{
                .volume_identifier = volume_identifier,
                .snapshot_identifier = snapshot.snapshot_identifier,
                .volume = VolumeName(volume_identifier),
                .device = snapshot.device,
            };
        }) |
        std::ranges::to<std::vector<AvailableBaseline>>();
}

[[nodiscard]] auto QueryBaselineSnapshot(
    const GUID &snapshot_identifier) -> std::vector<AvailableBaseline> {
    auto snapshot = std::move(
        devicefs::vshadow::QuerySnapshotProperties(
            std::array{snapshot_identifier}).front());
    if (!snapshot) {
        return {};
    }
    return {{
        .volume_identifier = winrt::guid{
            snapshot->original_volume.substr(11, 36)},
        .snapshot_identifier = snapshot_identifier,
        .volume = std::move(snapshot->original_volume),
        .device = std::move(snapshot->device),
    }};
}

[[nodiscard]] auto SelectPayloadVolumes(
    const std::span<const AvailableBaseline> baselines,
    const std::span<const std::wstring> volume_override) {
    if (!volume_override.empty()) {
        return std::vector<std::wstring>{
            volume_override.begin(), volume_override.end()};
    }
    return baselines |
        std::views::transform(&AvailableBaseline::volume) |
        std::ranges::to<std::vector<std::wstring>>();
}

[[nodiscard]] auto SameVolume(
    const std::wstring_view left,
    const std::wstring_view right) {
    return CompareStringOrdinal(
        left.data(), wil::safe_cast_failfast<int>(left.size()),
        right.data(), wil::safe_cast_failfast<int>(right.size()),
        TRUE) == CSTR_EQUAL;
}

[[nodiscard]] auto AssociateSnapshotIntervals(
    const std::span<const AvailableBaseline> baselines,
    const devicefs::vshadow::SnapshotSet &payload) {
    auto result = std::vector<SnapshotInterval>{};
    for (const auto &current : payload.snapshots) {
        const auto baseline = std::ranges::find_if(
            baselines,
            [&](const AvailableBaseline &candidate) {
                return SameVolume(
                    candidate.volume, current.original_volume);
            });
        if (baseline == baselines.end()) {
            continue;
        }
        result.push_back({
            .volume_identifier = baseline->volume_identifier,
            .baseline_snapshot_identifier = baseline->snapshot_identifier,
            .payload_snapshot_identifier = current.identifier,
            .volume = current.original_volume,
            .baseline_device = baseline->device,
            .payload_device = current.device,
        });
    }
    return result;
}

auto PrintUnavailableSnapshots(const std::size_t count) {
    if (count == 0) {
        return;
    }
    const auto &output = devicefs::WriteToStream(
        std::cout,
        "{} selected volume(s) had no retained snapshot and were skipped.\n",
        count);
    if (!output) {
        throw std::runtime_error(
            "could not write incremental snapshot availability");
    }
}

[[nodiscard]] constexpr auto VolumeBlockCount(
    const std::uint64_t volume_size) {
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

[[nodiscard]] auto BuildVolumeReport(
    const SnapshotInterval &interval) {
    auto map = [&]()
        -> std::expected<DirtyBlockMap, std::string> {
        try {
            return BuildDirtyBlockMap(
                interval.baseline_snapshot_identifier,
                interval.baseline_device,
                interval.payload_device);
        } catch (const std::runtime_error &error) {
            return std::unexpected{std::string{error.what()}};
        }
    }();
    return VolumeReport{
        .volume_identifier = interval.volume_identifier,
        .baseline_snapshot_identifier =
            interval.baseline_snapshot_identifier,
        .payload_snapshot_identifier =
            interval.payload_snapshot_identifier,
        .volume = interval.volume,
        .baseline_device = interval.baseline_device,
        .payload_device = interval.payload_device,
        .map = std::move(map),
    };
}

[[nodiscard]] auto BuildDirtyBlockReports(
    const HANDLE cancellation_event,
    const std::span<const SnapshotInterval> intervals) {
    const auto successor_volumes = intervals |
        std::views::transform(&SnapshotInterval::volume) |
        std::ranges::to<std::vector<std::wstring>>();
    auto reports = std::vector<VolumeReport>{};

    // C only makes payload B nonlatest; it is not a map input.
    const auto exit_code = devicefs::vshadow::Run(
        cancellation_event, false, successor_volumes,
        [&](const devicefs::vshadow::SnapshotSet &) {
            for (const auto &interval : intervals) {
                if (internal::CancellationRequested(cancellation_event)) {
                    return internal::kCancelledExitCode;
                }
                reports.push_back(BuildVolumeReport(interval));
            }
            return kTemporarySnapshotsComplete;
        });
    return DirtyBlockReportResult{
        .exit_code = exit_code,
        .reports = std::move(reports),
    };
}

using SyntheticBackupDevice =
    devicefs::SyntheticBackupBlockDevice<
        devicefs::WindowsBlockDevice>;
using SyntheticBackupServer =
    devicefs::RpcBlockDeviceServer<SyntheticBackupDevice>;

enum class BackupViewPreparation {
    Ready,
    OptimizationUnavailable,
    Failed,
};

[[nodiscard]] auto AddBackupViews(
    const VolumeReport &report,
    SyntheticBackupServer::Devices &devices,
    std::vector<internal::DeviceFsSource> &synthetic_sources,
    std::vector<internal::DeviceFsSource> &real_sources,
    std::vector<FilesystemVerificationVolume> &verification_volumes,
    std::ostream &output) {
    const auto volume_identifier =
        winrt::to_hstring(report.volume_identifier);
    if (!report.map) {
        devicefs::WriteToStream(output,
            L"\n  Volume ID: {}\n",
            std::wstring_view{volume_identifier});
        devicefs::WriteToStream(output,
            "    Not exposed: dirty map unavailable: {}\n",
            report.map.error());
        return BackupViewPreparation::OptimizationUnavailable;
    }

    auto synthetic = [&]() -> std::optional<SyntheticBackupDevice> {
        try {
            auto baseline = devicefs::WindowsBlockDevice::FromFilename(
                std::filesystem::path{report.baseline_device},
                true, true, true, kBaselineDescription);
            auto payload = devicefs::WindowsBlockDevice::FromFilename(
                std::filesystem::path{report.payload_device},
                true, true, true, kPayloadDescription);
            return SyntheticBackupDevice::FromBlockDevices(
                std::move(baseline), std::move(payload),
                devicefs::vss::kBlockSize,
                report.map->block_offsets);
        } catch (const std::runtime_error &error) {
            devicefs::WriteToStream(output,
                L"\n  Volume ID: {}\n",
                std::wstring_view{volume_identifier});
            devicefs::WriteToStream(output,
                "    Not exposed: {}\n", error.what());
            return std::nullopt;
        }
    }();
    if (!synthetic) {
        return BackupViewPreparation::Failed;
    }

    auto filename = internal::VolumeImageName(
        report.volume, L".vhdx");
    const auto baseline_identifier =
        winrt::to_hstring(report.baseline_snapshot_identifier);
    const auto payload_identifier =
        winrt::to_hstring(report.payload_snapshot_identifier);
    devicefs::WriteToStream(output,
        L"\n  File: {}\n"
        L"    Volume ID: {}\n"
        L"    Snapshot A: {}\n"
        L"      Device: {}\n"
        L"    Snapshot B: {}\n"
        L"      Device: {}\n"
        L"    RPC symbol: {}\n",
        filename,
        std::wstring_view{volume_identifier},
        std::wstring_view{baseline_identifier},
        report.baseline_device,
        std::wstring_view{payload_identifier},
        report.payload_device,
        filename);
    devicefs::WriteToStream(output,
        "    Dirty map: {} 16-KiB block(s)\n",
        report.map->block_offsets.size());

    synthetic_sources.push_back({
        .name = filename,
        .source = filename,
    });
    real_sources.push_back({
        .name = filename,
        .source = report.payload_device,
    });
    verification_volumes.push_back({
        .volume_identifier = report.volume_identifier,
        .payload_snapshot_identifier =
            report.payload_snapshot_identifier,
        .filename = filename,
    });
    devices.emplace_back(
        std::move(filename), std::move(*synthetic));
    return BackupViewPreparation::Ready;
}

[[nodiscard]] auto RunBackupViewDiagnostics(
    const HANDLE cancellation_event,
    const std::span<const VolumeReport> reports,
    const std::wstring_view mount_prefix,
    const std::optional<double> verification_percentage,
    const std::size_t unavailable_snapshots) -> int {
    auto &output = verification_percentage
        ? devicefs::WriteToStream(std::cout,
            "Synthetic and real backup VHDX verification:\n")
        : devicefs::WriteToStream(std::cout,
            "Synthetic and real backup VHDX exposure:\n");
    auto devices = SyntheticBackupServer::Devices{};
    auto synthetic_sources =
        std::vector<internal::DeviceFsSource>{};
    auto real_sources = std::vector<internal::DeviceFsSource>{};
    auto verification_volumes =
        std::vector<FilesystemVerificationVolume>{};
    auto optimization_unavailable = unavailable_snapshots;
    auto preparation_failures = 0uz;
    for (const auto &report : reports) {
        switch (AddBackupViews(report, devices,
            synthetic_sources, real_sources, verification_volumes,
            output)) {
        case BackupViewPreparation::Ready:
            break;
        case BackupViewPreparation::OptimizationUnavailable:
            ++optimization_unavailable;
            break;
        case BackupViewPreparation::Failed:
            ++preparation_failures;
            break;
        }
    }
    const auto synthetic_mount =
        std::format(L"{}\\synthetic", mount_prefix);
    const auto real_mount =
        std::format(L"{}\\real", mount_prefix);
    if (devices.empty()) {
        if (verification_percentage) {
            return VerifyFilesystemViews(
                cancellation_event, verification_volumes,
                synthetic_mount, real_mount,
                *verification_percentage,
                optimization_unavailable, preparation_failures);
        }
        throw std::runtime_error(
            "no synthetic backup VHDX could be exposed");
    }

    auto endpoint = std::format(
        L"devicefs-block-device-{}", internal::UniqueName());
    auto suspended_synthetic = internal::StartDeviceFs(
        internal::DeviceFsStartRequest{
            .sources = synthetic_sources,
            .mount_target = synthetic_mount,
            .rpc_endpoint = endpoint,
            .vhdx = true,
        });
    auto terminate_synthetic = wil::scope_exit([&] {
        static_cast<void>(TerminateProcess(
            suspended_synthetic.process.hProcess,
            ERROR_PROCESS_ABORTED));
    });
    // The child owners must be destroyed before the RPC server because
    // unmounting the synthetic VHDX can issue final block-device reads.
    auto server = SyntheticBackupServer::Start(
        endpoint, suspended_synthetic.process.dwProcessId,
        std::move(devices));
    internal::ResumeDeviceFs(suspended_synthetic);
    auto synthetic_devicefs = internal::DeviceFsChild{
        std::move(suspended_synthetic)};
    terminate_synthetic.release();
    auto real_devicefs = internal::DeviceFsChild{
        internal::StartDeviceFs(
            internal::DeviceFsStartRequest{
                .sources = real_sources,
                .mount_target = real_mount,
                .vhdx = true,
            })};
    if (!internal::WaitForDeviceFs(
            synthetic_devicefs.Process(), cancellation_event) ||
        !internal::WaitForDeviceFs(
            real_devicefs.Process(), cancellation_event)) {
        if (verification_percentage) {
            return VerifyFilesystemViews(
                cancellation_event, verification_volumes,
                synthetic_mount, real_mount,
                *verification_percentage,
                optimization_unavailable, preparation_failures);
        }
        return 0;
    }

    if (verification_percentage) {
        devicefs::WriteToStream(
            output,
            L"\nMounted {} synthetic backup VHDX file(s) at {}.\n"
            L"Mounted the corresponding real-B VHDX files at {}.\n",
            synthetic_sources.size(), synthetic_mount, real_mount);
        output.flush();
        const auto result = VerifyFilesystemViews(
            cancellation_event, verification_volumes,
            synthetic_mount, real_mount,
            *verification_percentage,
            optimization_unavailable, preparation_failures);
        if (result != 0) {
            return result;
        }
        synthetic_devicefs.Stop();
        real_devicefs.Stop();
        return 0;
    }

    devicefs::WriteToStream(
        output,
        L"\nMounted {} synthetic backup VHDX file(s) at {}.\n"
        L"Mounted the corresponding real VHDX files at {}.\n"
        L"Press Ctrl+C to stop.\n",
        synthetic_sources.size(), synthetic_mount, real_mount);
    output.flush();
    const auto processes = std::array{
        &synthetic_devicefs.Process(),
        &real_devicefs.Process(),
    };
    const auto exit = internal::WaitForDeviceFsExitOrCancellation(
        processes, cancellation_event);
    if (exit) {
        constexpr auto kChildNames =
            std::array{"synthetic", "real"};
        throw std::runtime_error(std::format(
            "the {} devicefs child exited while exposing backup "
            "views with code {}.",
            kChildNames[exit->process_index], exit->exit_code));
    }
    synthetic_devicefs.Stop();
    real_devicefs.Stop();
    return 0;
}

struct UnexpectedDifference {
    std::uint64_t block_offset;
    std::uint64_t byte_offset;
    unsigned char baseline;
    unsigned char payload;
};

struct ComparisonObservation {
    std::uint64_t compared_bytes = 0;
    std::uint64_t differing_bytes = 0;
    std::uint64_t differing_blocks = 0;
    std::uint64_t uncovered_bytes = 0;
    std::uint64_t uncovered_blocks = 0;
    std::optional<UnexpectedDifference> first_uncovered;
    std::vector<std::uint64_t> uncovered_clusters;
    std::string failure;
};

class VerificationState {
  public:
    [[nodiscard]] auto Failed() const noexcept {
        return failed_.load(std::memory_order_acquire);
    }

    auto Record(
        const std::uint64_t compared_bytes,
        const ComparisonObservation &observation) -> void {
        const auto lock = std::scoped_lock{mutex_};
        observation_.compared_bytes += compared_bytes;
        observation_.differing_bytes += observation.differing_bytes;
        observation_.differing_blocks += observation.differing_blocks;
        observation_.uncovered_bytes += observation.uncovered_bytes;
        observation_.uncovered_blocks += observation.uncovered_blocks;
        observation_.uncovered_clusters.append_range(
            observation.uncovered_clusters);
        if (observation.first_uncovered &&
            (!observation_.first_uncovered ||
                (observation.first_uncovered->byte_offset <
                    observation_.first_uncovered->byte_offset))) {
            observation_.first_uncovered = observation.first_uncovered;
        }
    }

    auto Fail(const std::string_view error) -> void {
        const auto lock = std::scoped_lock{mutex_};
        if (observation_.failure.empty()) {
            observation_.failure = error;
        }
        failed_.store(true, std::memory_order_release);
    }

    [[nodiscard]] auto Snapshot() const {
        const auto lock = std::scoped_lock{mutex_};
        return observation_;
    }

  private:
    mutable std::mutex mutex_;
    ComparisonObservation observation_;
    std::atomic_bool failed_ = false;
};

struct VerificationAllocation {
    devicefs::SnapshotAllocationBitmap baseline;
    devicefs::SnapshotAllocationBitmap payload;
};

enum class SnapshotEndpoint {
    Baseline,
    Payload,
};

struct SnapshotStreamMatch {
    std::uint64_t cluster;
    DWORD flags;
    std::wstring name;
};

struct EndpointStreamOwnership {
    DWORD flags = 0;
    std::set<std::uint64_t> clusters;
};

struct StreamOwnership {
    EndpointStreamOwnership baseline;
    EndpointStreamOwnership payload;
    bool printed_during_progress = false;
};

struct SnapshotOwnershipResult {
    std::set<std::uint64_t> no_stream_clusters;
    std::set<std::uint64_t> failed_clusters;
    std::string failure;
};

struct ClusterOwnershipCache {
    std::map<std::wstring, StreamOwnership> streams;
    SnapshotOwnershipResult baseline;
    SnapshotOwnershipResult payload;
    std::set<std::uint64_t> queried_clusters;
};

struct VerificationJob {
    std::reference_wrapper<const VolumeReport> volume;
    std::uint64_t volume_size;
    std::unique_ptr<VerificationState> state;
    std::optional<VerificationAllocation> allocation;
    ClusterOwnershipCache ownership;
    // Futures are destroyed first so an exceptional unwind cannot release
    // state or allocation data while a worker still refers to them.
    std::vector<std::future<void>> workers;
};

[[nodiscard]] auto OpenVerificationSnapshot(
    const std::wstring_view source,
    const std::string_view description) {
    const auto path = std::filesystem::path{source};
    auto result = wil::unique_hfile{CreateFileW(
        path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
        nullptr)};
    if (!result) {
        WinError("could not open {} for incremental verification",
            description);
    }

    // Exact reads remain authoritative. This request only permits access to
    // the final raw-volume sectors where the filesystem supports it.
    auto returned = DWORD{};
    static_cast<void>(DeviceIoControl(
        result.get(), FSCTL_ALLOW_EXTENDED_DASD_IO,
        nullptr, 0, nullptr, 0, &returned, nullptr));
    return result;
}

[[nodiscard]] auto LoadVerificationAllocation(
    const VolumeReport &volume) {
    auto baseline = devicefs::LoadSnapshotAllocationBitmap(
        volume.baseline_device, kBaselineDescription);
    auto payload = devicefs::LoadSnapshotAllocationBitmap(
        volume.payload_device, kPayloadDescription);
    if (baseline.VolumeSize() != payload.VolumeSize()) {
        throw std::runtime_error(
            "the retained and new snapshot volume sizes do not match");
    }
    return VerificationAllocation{
        .baseline = std::move(baseline),
        .payload = std::move(payload),
    };
}

[[nodiscard]] auto AllocateComparisonBuffer() {
    auto result = wil::unique_virtualalloc_ptr<unsigned char>{
        static_cast<unsigned char *>(VirtualAlloc(
            nullptr, kComparisonChunkSize,
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE))};
    if (!result) {
        WinError("could not allocate an incremental verification buffer");
    }
    return result;
}

auto SeekVerificationSnapshot(
    const HANDLE snapshot,
    const std::uint64_t offset,
    const std::string_view description) -> void {
    const auto position = LARGE_INTEGER{
        .QuadPart = wil::safe_cast_failfast<LONGLONG>(offset),
    };
    if (!SetFilePointerEx(snapshot, position, nullptr, FILE_BEGIN)) {
        WinError("could not seek {} for incremental verification",
            description);
    }
}

auto ReadVerificationChunk(
    const HANDLE snapshot,
    const std::span<unsigned char> destination,
    const std::uint64_t offset,
    const std::string_view description) -> void {
    const auto requested =
        wil::safe_cast_failfast<DWORD>(destination.size());
    auto completed = DWORD{};
    if (!ReadFile(snapshot, destination.data(), requested,
            &completed, nullptr)) {
        WinError("could not read {} at offset 0x{:x} "
            "during incremental verification", description, offset);
    }
    if (completed != requested) {
        throw std::runtime_error(std::format(
            "the incremental verification read from {} at offset 0x{:x} "
            "completed with {} of {} bytes",
            description, offset, completed, requested));
    }
}

using DirtyBlockIterator =
    std::span<const std::uint64_t>::iterator;

auto AppendDifferingClusters(
    const std::span<const unsigned char> baseline,
    const std::span<const unsigned char> payload,
    const std::uint64_t block_offset,
    const std::uint32_t cluster_size,
    std::vector<std::uint64_t> &clusters) -> void {
    const auto block_end = block_offset + baseline.size();
    const auto first_cluster = block_offset / cluster_size;
    const auto past_last_cluster =
        ((block_end - 1) / cluster_size) + 1;
    for (const auto cluster :
        std::views::iota(first_cluster, past_last_cluster)) {
        const auto cluster_start = cluster * cluster_size;
        const auto cluster_end = cluster_start + cluster_size;
        const auto start = std::max(block_offset, cluster_start);
        const auto end = std::min(block_end, cluster_end);
        const auto relative = wil::safe_cast_failfast<std::size_t>(
            start - block_offset);
        const auto size = wil::safe_cast_failfast<std::size_t>(end - start);
        if (!std::ranges::equal(
                baseline.subspan(relative, size),
                payload.subspan(relative, size))) {
            clusters.push_back(cluster);
        }
    }
}

[[nodiscard]] auto CompareVerificationChunk(
    const std::span<const unsigned char> baseline,
    const std::span<const unsigned char> payload,
    const std::uint64_t chunk_offset,
    const std::uint32_t cluster_size,
    const bool verify_coverage,
    DirtyBlockIterator &next_dirty,
    const DirtyBlockIterator dirty_end) {
    auto result = ComparisonObservation{};
    if (std::ranges::equal(baseline, payload)) {
        return result;
    }

    for (auto relative = 0uz; relative < baseline.size();
        relative += kMapBlockSize) {
        const auto size = std::min(
            kMapBlockSize, baseline.size() - relative);
        const auto baseline_block = baseline.subspan(relative, size);
        const auto payload_block = payload.subspan(relative, size);
        const auto mismatch = std::ranges::mismatch(
            baseline_block, payload_block);
        if (mismatch.in1 == baseline_block.end()) {
            continue;
        }

        const auto differing_bytes = std::transform_reduce(
            baseline_block.begin(), baseline_block.end(),
            payload_block.begin(), std::uint64_t{}, std::plus{},
            std::not_equal_to{});
        result.differing_bytes += differing_bytes;
        ++result.differing_blocks;
        if (!verify_coverage) {
            continue;
        }

        const auto block_offset = chunk_offset + relative;
        while ((next_dirty != dirty_end) &&
            (*next_dirty < block_offset)) {
            ++next_dirty;
        }
        if ((next_dirty != dirty_end) &&
            (*next_dirty == block_offset)) {
            ++next_dirty;
            continue;
        }
        result.uncovered_bytes += differing_bytes;
        ++result.uncovered_blocks;
        AppendDifferingClusters(
            baseline_block, payload_block, block_offset,
            cluster_size, result.uncovered_clusters);
        if (!result.first_uncovered) {
            const auto relative_byte =
                wil::safe_cast_failfast<std::uint64_t>(
                    std::ranges::distance(
                        baseline_block.begin(), mismatch.in1));
            result.first_uncovered = UnexpectedDifference{
                .block_offset = block_offset,
                .byte_offset = block_offset + relative_byte,
                .baseline = *mismatch.in1,
                .payload = *mismatch.in2,
            };
        }
    }
    return result;
}

auto VerifySnapshotRange(
    VerificationState &state,
    const VolumeReport &volume,
    const VerificationAllocation &allocation,
    const HANDLE cancellation_event,
    const std::uint64_t range_start,
    const std::uint64_t range_end) -> void {
    try {
        if (state.Failed() ||
            internal::CancellationRequested(cancellation_event)) {
            return;
        }
        auto baseline = OpenVerificationSnapshot(
            volume.baseline_device, kBaselineDescription);
        auto payload = OpenVerificationSnapshot(
            volume.payload_device, kPayloadDescription);
        auto baseline_storage = AllocateComparisonBuffer();
        auto payload_storage = AllocateComparisonBuffer();
        SeekVerificationSnapshot(
            baseline.get(), range_start, kBaselineDescription);
        SeekVerificationSnapshot(
            payload.get(), range_start, kPayloadDescription);

        const auto dirty_blocks = volume.map
            ? std::span<const std::uint64_t>{
                volume.map->block_offsets}
            : std::span<const std::uint64_t>{};
        const auto cluster_size = allocation.baseline.ClusterSize();
        auto next_dirty = std::ranges::lower_bound(
            dirty_blocks, range_start);
        for (auto offset = range_start; offset < range_end;
            offset += kComparisonChunkSize) {
            if (state.Failed() ||
                internal::CancellationRequested(cancellation_event)) {
                return;
            }
            const auto size = wil::safe_cast_failfast<std::size_t>(
                std::min<std::uint64_t>(
                    kComparisonChunkSize, range_end - offset));
            const auto baseline_chunk =
                std::span{baseline_storage.get(), kComparisonChunkSize}
                    .first(size);
            const auto payload_chunk =
                std::span{payload_storage.get(), kComparisonChunkSize}
                    .first(size);
            ReadVerificationChunk(
                baseline.get(), baseline_chunk,
                offset, kBaselineDescription);
            ReadVerificationChunk(
                payload.get(), payload_chunk,
                offset, kPayloadDescription);
            // Raw free-cluster contents are not stable across snapshot
            // devices. Compare the synthetic views that DeviceFs backs up.
            allocation.baseline.SynthesizeFreeClusters(
                baseline_chunk, offset);
            allocation.payload.SynthesizeFreeClusters(
                payload_chunk, offset);
            state.Record(size, CompareVerificationChunk(
                baseline_chunk, payload_chunk, offset,
                cluster_size,
                volume.map.has_value(),
                next_dirty, dirty_blocks.end()));
        }
    } catch (const std::runtime_error &error) {
        state.Fail(error.what());
    }
}

struct VerificationRange {
    std::uint64_t start;
    std::uint64_t end;
};

[[nodiscard]] constexpr auto VerificationWorkerRange(
    const std::uint64_t volume_size,
    const std::size_t worker) {
    const auto chunk_count =
        volume_size / kComparisonChunkSize +
        ((volume_size % kComparisonChunkSize) != 0);
    const auto first_chunk =
        (chunk_count * worker) / kVerificationWorkerCount;
    const auto past_last_chunk =
        (chunk_count * (worker + 1)) / kVerificationWorkerCount;
    return VerificationRange{
        .start = first_chunk * kComparisonChunkSize,
        .end = std::min<std::uint64_t>(
            past_last_chunk * kComparisonChunkSize, volume_size),
    };
}

[[nodiscard]] auto StartVerificationWorker(
    VerificationJob &job,
    const HANDLE cancellation_event,
    const VerificationRange range) {
    try {
        job.workers.push_back(std::async(
            std::launch::async,
            VerifySnapshotRange,
            std::ref(*job.state),
            job.volume,
            std::cref(*job.allocation),
            cancellation_event,
            range.start, range.end));
        return true;
    } catch (const std::system_error &error) {
        job.state->Fail(error.what());
        return false;
    }
}

auto StartVerificationWorkers(
    VerificationJob &job,
    const HANDLE cancellation_event) {
    if (!job.allocation) {
        return;
    }
    for (auto worker = 0uz;
        worker < kVerificationWorkerCount; ++worker) {
        if (job.state->Failed() ||
            internal::CancellationRequested(cancellation_event)) {
            return;
        }
        const auto range = VerificationWorkerRange(
            job.volume_size, worker);
        if (range.start == range.end) {
            continue;
        }
        if (!StartVerificationWorker(
                job, cancellation_event, range)) {
            return;
        }
    }
}

[[nodiscard]] auto LookupSnapshotStreams(
    const std::wstring_view snapshot,
    const std::string_view description,
    const std::span<const std::uint64_t> clusters) {
    const auto api_clusters = clusters |
        std::views::transform([](const auto cluster) {
            return LARGE_INTEGER{
                .QuadPart = wil::safe_cast_failfast<LONGLONG>(cluster),
            };
        }) |
        std::ranges::to<std::vector<LARGE_INTEGER>>();
    const auto api_cluster_bytes =
        std::as_bytes(std::span{api_clusters});
    constexpr auto input_header_size =
        offsetof(LOOKUP_STREAM_FROM_CLUSTER_INPUT, Cluster);
    const auto input_size =
        input_header_size + api_cluster_bytes.size();
    if (!std::in_range<DWORD>(input_size)) {
        throw std::runtime_error(
            "the uncovered-cluster lookup input is too large");
    }
    const auto api_input_size =
        wil::safe_cast_failfast<DWORD>(input_size);
    const auto input_header = LOOKUP_STREAM_FROM_CLUSTER_INPUT{
        .Flags = 0,
        .NumberOfClusters =
            wil::safe_cast_failfast<DWORD>(clusters.size()),
    };
    auto input_storage = std::vector<std::byte>(input_size);
    std::memcpy(
        input_storage.data(), &input_header, input_header_size);
    std::memcpy(
        input_storage.data() + input_header_size,
        api_cluster_bytes.data(), api_cluster_bytes.size());

    auto device = OpenVerificationSnapshot(snapshot, description);
    const auto lookup = [&](
        void *const output, const DWORD output_size, DWORD &returned) {
        return DeviceIoControl(device.get(),
            FSCTL_LOOKUP_STREAM_FROM_CLUSTER,
            input_storage.data(),
            api_input_size,
            output, output_size, &returned, nullptr);
    };

    auto probe = LOOKUP_STREAM_FROM_CLUSTER_OUTPUT{};
    auto returned = DWORD{};
    if (!lookup(&probe, sizeof(probe), returned)) {
        const auto error = GetLastError();
        if (error != ERROR_MORE_DATA) {
            WinError("FSCTL_LOOKUP_STREAM_FROM_CLUSTER failed for {}",
                description, ExplicitWin32Error{error});
        }
    }
    if (probe.NumberOfMatches == 0) {
        return std::vector<SnapshotStreamMatch>{};
    }

    auto output_storage =
        std::vector<std::byte>(probe.BufferSizeRequired);
    if (!lookup(
            output_storage.data(), probe.BufferSizeRequired, returned)) {
        WinError("FSCTL_LOOKUP_STREAM_FROM_CLUSTER failed for {}",
            description);
    }

    auto output = LOOKUP_STREAM_FROM_CLUSTER_OUTPUT{};
    std::memcpy(&output, output_storage.data(), sizeof(output));
    auto result = std::vector<SnapshotStreamMatch>{};
    result.reserve(output.NumberOfMatches);
    if (output.Offset == 0) {
        return result;
    }
    auto entries = std::span{output_storage}.first(returned)
        .subspan(output.Offset);
    constexpr auto entry_header_size =
        offsetof(LOOKUP_STREAM_FROM_CLUSTER_ENTRY, FileName);
    while (true) {
        auto entry = LOOKUP_STREAM_FROM_CLUSTER_ENTRY{};
        std::memcpy(&entry, entries.data(), entry_header_size);
        const auto entry_size = entry.OffsetToNext == 0
            ? entries.size() : entry.OffsetToNext;
        const auto name_characters =
            (entry_size - entry_header_size) /
            sizeof(std::wstring::value_type);
        auto name = std::wstring(name_characters, L'\0');
        std::memcpy(name.data(), entries.data() + entry_header_size,
            name.size() * sizeof(std::wstring::value_type));
        // FileName is a null-terminated trailing array whose SDK declaration
        // exposes only its first element.
        name.resize(name.find(L'\0'));
        result.push_back({
            .cluster = wil::safe_cast_failfast<std::uint64_t>(
                entry.Cluster.QuadPart),
            .flags = entry.Flags,
            .name = std::move(name),
        });
        if (entry.OffsetToNext == 0) {
            break;
        }
        entries = entries.subspan(entry.OffsetToNext);
    }
    return result;
}

auto ResolveSnapshotOwnership(
    ClusterOwnershipCache &ownership_cache,
    const SnapshotEndpoint endpoint,
    const std::wstring_view snapshot,
    const std::span<const std::uint64_t> clusters) -> void {
    const auto description = endpoint == SnapshotEndpoint::Baseline
        ? kBaselineDescription : kPayloadDescription;
    auto &snapshot_result = endpoint == SnapshotEndpoint::Baseline
        ? ownership_cache.baseline : ownership_cache.payload;
    try {
        auto unmatched = std::set<std::uint64_t>{
            clusters.begin(), clusters.end()};
        for (auto &stream :
            LookupSnapshotStreams(snapshot, description, clusters)) {
            auto &ownership = ownership_cache.streams[stream.name];
            auto &endpoint_ownership =
                endpoint == SnapshotEndpoint::Baseline
                    ? ownership.baseline : ownership.payload;
            endpoint_ownership.flags |= stream.flags;
            endpoint_ownership.clusters.insert(stream.cluster);
            unmatched.erase(stream.cluster);
        }
        snapshot_result.no_stream_clusters.insert_range(unmatched);
    } catch (const std::runtime_error &error) {
        snapshot_result.failed_clusters.insert_range(clusters);
        if (snapshot_result.failure.empty()) {
            snapshot_result.failure = error.what();
        }
    }
}

auto UpdateClusterOwnership(
    VerificationJob &job,
    const std::span<const std::uint64_t> observed_clusters) -> void {
    auto new_clusters = std::vector<std::uint64_t>{};
    for (const auto cluster : observed_clusters) {
        if (job.ownership.queried_clusters.insert(cluster).second) {
            new_clusters.push_back(cluster);
        }
    }
    if (new_clusters.empty()) {
        return;
    }
    ResolveSnapshotOwnership(
        job.ownership, SnapshotEndpoint::Baseline,
        job.volume.get().baseline_device, new_clusters);
    ResolveSnapshotOwnership(
        job.ownership, SnapshotEndpoint::Payload,
        job.volume.get().payload_device, new_clusters);
}

[[nodiscard]] constexpr auto VerificationPercentage(
    const std::uint64_t part,
    const std::uint64_t whole) {
    return whole == 0
        ? 0.0
        : (static_cast<double>(part) * 100.0) /
            static_cast<double>(whole);
}

auto PrintEndpointClusters(
    std::ostream &output,
    const std::string_view endpoint,
    const std::set<std::uint64_t> &clusters,
    const std::optional<DWORD> flags = std::nullopt) noexcept -> void {
    if (clusters.empty()) {
        return;
    }
    devicefs::WriteToStream(output,
        "        {}: {} cluster(s)",
        endpoint, clusters.size());
    if (flags) {
        devicefs::WriteToStream(
            output, ", flags 0x{:08x}", *flags);
    }
    devicefs::WriteToStream(output, ", LCNs:");
    for (const auto cluster : clusters) {
        devicefs::WriteToStream(output, " 0x{:x}", cluster);
    }
    devicefs::WriteToStream(output, "\n");
}

auto PrintStreamOwnership(
    std::ostream &output,
    const std::wstring_view name,
    const StreamOwnership &ownership) noexcept -> void {
    devicefs::WriteToStream(output, L"      {}\n", name);
    PrintEndpointClusters(
        output, "A", ownership.baseline.clusters,
        ownership.baseline.flags);
    PrintEndpointClusters(
        output, "B", ownership.payload.clusters,
        ownership.payload.flags);
}

[[nodiscard]] auto PrintOwnershipUpdate(
    std::ostream &output,
    const GUID &volume_identifier,
    ClusterOwnershipCache &ownership) {
    auto wrote_heading = false;
    for (auto &[name, stream] : ownership.streams) {
        if (stream.printed_during_progress) {
            continue;
        }
        if (!wrote_heading) {
            const auto identifier =
                winrt::to_hstring(volume_identifier);
            devicefs::WriteToStream(output,
                L"  Volume ID: {}\n",
                std::wstring_view{identifier});
            devicefs::WriteToStream(
                output, "    NTFS ownership update:\n");
            wrote_heading = true;
        }
        PrintStreamOwnership(output, name, stream);
        stream.printed_during_progress = true;
    }
    return wrote_heading;
}

auto PrintClusterOwnership(
    std::ostream &output,
    const ClusterOwnershipCache &ownership,
    const std::uint32_t cluster_size) noexcept -> void {
    devicefs::WriteToStream(output,
        "    NTFS ownership of uncovered differences:\n"
        "      {} distinct {}-byte NTFS cluster(s) queried\n",
        ownership.queried_clusters.size(), cluster_size);
    for (const auto &[name, stream] : ownership.streams) {
        PrintStreamOwnership(output, name, stream);
    }

    if (!ownership.baseline.failure.empty()) {
        devicefs::WriteToStream(output,
            "      A ownership lookup failed: {}\n",
            ownership.baseline.failure);
        PrintEndpointClusters(
            output, "A unresolved", ownership.baseline.failed_clusters);
    }
    if (!ownership.payload.failure.empty()) {
        devicefs::WriteToStream(output,
            "      B ownership lookup failed: {}\n",
            ownership.payload.failure);
        PrintEndpointClusters(
            output, "B unresolved", ownership.payload.failed_clusters);
    }
    if (!ownership.baseline.no_stream_clusters.empty() ||
        !ownership.payload.no_stream_clusters.empty()) {
        devicefs::WriteToStream(
            output, "      No stream reported by NTFS:\n");
        PrintEndpointClusters(
            output, "A", ownership.baseline.no_stream_clusters);
        PrintEndpointClusters(
            output, "B", ownership.payload.no_stream_clusters);
    }
}

auto FlushVerificationProgress(std::ostream &output) -> void {
    if (!output) {
        throw std::runtime_error(
            "could not write incremental verification progress");
    }
    output.flush();
    if (!output) {
        throw std::runtime_error(
            "could not flush incremental verification progress");
    }
}

auto PrintVerificationProgress(
    const std::span<VerificationJob> jobs) {
    const auto observations = jobs |
        std::views::transform([](const VerificationJob &job) {
            return job.state->Snapshot();
        }) |
        std::ranges::to<std::vector<ComparisonObservation>>();
    auto &output = devicefs::WriteToStream(
        std::cout, "\nIncremental verification progress:\n");
    auto compared_total = std::uint64_t{};
    auto volume_total = std::uint64_t{};
    auto differing_total = std::uint64_t{};
    auto uncovered_total = std::uint64_t{};
    auto coverage_unavailable = std::size_t{};
    for (auto &&[job, observation] :
        std::views::zip(jobs, observations)) {
        const auto identifier =
            winrt::to_hstring(job.volume.get().volume_identifier);
        const auto volume_size = job.volume_size;
        compared_total += observation.compared_bytes;
        volume_total += volume_size;
        differing_total += observation.differing_bytes;
        uncovered_total += observation.uncovered_blocks;
        devicefs::WriteToStream(output,
            L"  Volume ID: {}\n",
            std::wstring_view{identifier});
        devicefs::WriteToStream(output,
            "    Compared: {} of {} bytes ({:.2f}%)\n"
            "    Differences observed: {} byte(s) in {} 16-KiB block(s)\n",
            observation.compared_bytes, volume_size,
            VerificationPercentage(
                observation.compared_bytes, volume_size),
            observation.differing_bytes,
            observation.differing_blocks);
        if (!job.volume.get().map) {
            ++coverage_unavailable;
            devicefs::WriteToStream(output,
                "    Dirty-map coverage unavailable: {}\n",
                job.volume.get().map.error());
        } else {
            devicefs::WriteToStream(output,
                "    Dirty-map coverage: {} differing block(s) "
                "were absent\n",
                observation.uncovered_blocks);
        }
        if (!observation.failure.empty()) {
            devicefs::WriteToStream(output,
                "    Verification unavailable: {}\n",
                observation.failure);
        }
    }
    devicefs::WriteToStream(output,
        "  Total: {} of {} bytes ({:.2f}%); {} differing byte(s), "
        "{} uncovered block(s); dirty-map coverage unavailable for "
        "{} volume(s)\n",
        compared_total, volume_total,
        VerificationPercentage(compared_total, volume_total),
        differing_total, uncovered_total, coverage_unavailable);
    FlushVerificationProgress(output);

    for (auto &&[job, observation] :
        std::views::zip(jobs, observations)) {
        if (observation.uncovered_clusters.empty()) {
            continue;
        }
        UpdateClusterOwnership(job, observation.uncovered_clusters);
        if (PrintOwnershipUpdate(
                output, job.volume.get().volume_identifier,
                job.ownership)) {
            FlushVerificationProgress(output);
        }
    }
}

struct VerificationSummary {
    std::size_t verified = 0;
    std::size_t coverage_failures = 0;
    std::size_t coverage_unavailable = 0;
    std::size_t incomplete = 0;
    std::uint64_t compared_bytes = 0;
    std::uint64_t volume_bytes = 0;
    std::uint64_t differing_bytes = 0;
    std::uint64_t differing_blocks = 0;
    std::uint64_t uncovered_bytes = 0;
    std::uint64_t uncovered_blocks = 0;
};

auto AddVerificationResult(
    VerificationSummary &summary,
    const ComparisonObservation &observation,
    const std::uint64_t volume_size,
    const bool complete,
    const bool coverage_available) noexcept {
    if (!complete) {
        ++summary.incomplete;
    }
    if (!coverage_available) {
        ++summary.coverage_unavailable;
    }
    if (observation.uncovered_blocks != 0) {
        ++summary.coverage_failures;
    } else if (complete && coverage_available) {
        ++summary.verified;
    }
    summary.compared_bytes += observation.compared_bytes;
    summary.volume_bytes += volume_size;
    summary.differing_bytes += observation.differing_bytes;
    summary.differing_blocks += observation.differing_blocks;
    summary.uncovered_bytes += observation.uncovered_bytes;
    summary.uncovered_blocks += observation.uncovered_blocks;
}

auto PrintVerificationResult(
    std::ostream &output,
    const VerificationJob &job,
    const ComparisonObservation &observation,
    const std::uint64_t volume_size,
    const bool complete,
    const bool cancelled) {
    const auto identifier =
        winrt::to_hstring(job.volume.get().volume_identifier);
    devicefs::WriteToStream(output,
        L"\n  Volume ID: {}\n",
        std::wstring_view{identifier});
    if (!observation.failure.empty()) {
        devicefs::WriteToStream(output,
            "    Status: failed after comparing {} byte(s): {}\n",
            observation.compared_bytes, observation.failure);
    } else if (complete) {
        devicefs::WriteToStream(output,
            "    Status: comparison complete\n");
    } else if (cancelled) {
        devicefs::WriteToStream(output,
            "    Status: cancelled after comparing {} byte(s)\n",
            observation.compared_bytes);
    } else {
        devicefs::WriteToStream(output,
            "    Status: incomplete after comparing {} byte(s)\n",
            observation.compared_bytes);
    }
    devicefs::WriteToStream(output,
        "    Compared: {} of {} bytes ({:.2f}%)\n"
        "    Differences: {} byte(s) in {} 16-KiB block(s)\n",
        observation.compared_bytes, volume_size,
        VerificationPercentage(
            observation.compared_bytes, volume_size),
        observation.differing_bytes,
        observation.differing_blocks);
    if (!job.volume.get().map) {
        devicefs::WriteToStream(output,
            "    Dirty-map candidates and coverage unavailable: {}\n",
            job.volume.get().map.error());
        return;
    }

    devicefs::WriteToStream(output,
        "    Dirty-map candidates: {} 16-KiB block(s)\n",
        job.volume.get().map->block_offsets.size());
    if (observation.uncovered_blocks != 0) {
        devicefs::WriteToStream(output,
            "    Coverage failure: {} differing byte(s) in {} "
            "block(s) were absent from the dirty map\n",
            observation.uncovered_bytes,
            observation.uncovered_blocks);
    } else if (complete) {
        devicefs::WriteToStream(output,
            "    Coverage: every observed differing block was present "
            "in the dirty map\n");
    } else {
        devicefs::WriteToStream(output,
            "    Coverage so far: no uncovered differing block was "
            "observed in the completed ranges\n");
    }
    if (observation.first_uncovered) {
        devicefs::WriteToStream(output,
            "    First uncovered difference: block 0x{:x}, byte "
            "0x{:x}, A=0x{:02x}, B=0x{:02x}\n",
            observation.first_uncovered->block_offset,
            observation.first_uncovered->byte_offset,
            observation.first_uncovered->baseline,
            observation.first_uncovered->payload);
    }
    if (!job.ownership.queried_clusters.empty()) {
        PrintClusterOwnership(output, job.ownership,
            job.allocation->baseline.ClusterSize());
    }
}

[[nodiscard]] auto PrintVerificationSummary(
    const std::span<const VerificationJob> jobs,
    const std::size_t unprocessed_volumes,
    const std::size_t unavailable_snapshots,
    const bool cancelled) {
    auto &output = devicefs::WriteToStream(
        std::cout, "\nIncremental verification results:\n");
    auto summary = VerificationSummary{};
    for (const auto &job : jobs) {
        const auto observation = job.state->Snapshot();
        const auto volume_size = job.volume_size;
        const auto complete = observation.failure.empty() &&
            (observation.compared_bytes == volume_size);
        AddVerificationResult(
            summary, observation, volume_size, complete,
            job.volume.get().map.has_value());
        PrintVerificationResult(
            output, job, observation, volume_size, complete, cancelled);
    }
    devicefs::WriteToStream(output,
        "\nSummary: {} volume(s) verified, {} with uncovered differences, "
        "{} with dirty-map coverage unavailable, {} incomplete, "
        "{} not reached, {} selected without an available retained "
        "snapshot; {} of {} bytes compared "
        "({:.2f}%), {} differing byte(s) in {} block(s), {} uncovered "
        "byte(s) in {} block(s).\n",
        summary.verified, summary.coverage_failures,
        summary.coverage_unavailable, summary.incomplete,
        unprocessed_volumes, unavailable_snapshots,
        summary.compared_bytes, summary.volume_bytes,
        VerificationPercentage(
            summary.compared_bytes, summary.volume_bytes),
        summary.differing_bytes, summary.differing_blocks,
        summary.uncovered_bytes, summary.uncovered_blocks);
    if (!output) {
        throw std::runtime_error(
            "could not write the incremental verification results");
    }
    if (cancelled) {
        return internal::kCancelledExitCode;
    }
    return ((summary.incomplete != 0) ||
        (summary.coverage_failures != 0) ||
        (unprocessed_volumes != 0)) ? 1 : 0;
}

[[nodiscard]] auto RunIncrementalVerification(
    const HANDLE cancellation_event,
    const std::span<const VolumeReport> volumes,
    const std::size_t unprocessed_volumes,
    const std::size_t unavailable_snapshots) -> int {
    auto jobs = std::vector<VerificationJob>{};
    for (const auto &volume : volumes) {
        if (internal::CancellationRequested(cancellation_event)) {
            break;
        }
        auto state = std::make_unique<VerificationState>();
        auto volume_size = std::uint64_t{};
        auto allocation = std::optional<VerificationAllocation>{};
        try {
            allocation.emplace(
                LoadVerificationAllocation(volume));
            volume_size = allocation->baseline.VolumeSize();
        } catch (const std::runtime_error &error) {
            state->Fail(error.what());
        }
        jobs.push_back({
            .volume = std::cref(volume),
            .volume_size = volume_size,
            .state = std::move(state),
            .allocation = std::move(allocation),
        });
    }

    for (auto &job : jobs) {
        StartVerificationWorkers(job, cancellation_event);
    }

    const auto all_workers_ready = [&] {
        return std::ranges::all_of(
            jobs, [](const VerificationJob &job) {
                return std::ranges::all_of(
                    job.workers, [](const std::future<void> &worker) {
                        return worker.wait_for(0s) ==
                            std::future_status::ready;
                    });
            });
    };
    auto next_progress =
        std::chrono::steady_clock::now() +
        kProgressReportInterval;
    while (!all_workers_ready()) {
        std::this_thread::sleep_for(kWorkerPollInterval);
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_progress) {
            PrintVerificationProgress(jobs);
            next_progress = std::chrono::steady_clock::now() +
                kProgressReportInterval;
        }
    }
    for (auto &job : jobs) {
        for (auto &worker : job.workers) {
            worker.get();
        }
        const auto observation = job.state->Snapshot();
        UpdateClusterOwnership(job, observation.uncovered_clusters);
    }

    return PrintVerificationSummary(
        jobs,
        unprocessed_volumes + volumes.size() - jobs.size(),
        unavailable_snapshots,
        internal::CancellationRequested(cancellation_event));
}

[[nodiscard]] auto RunPayloadDiagnostics(
    const HANDLE cancellation_event,
    const std::span<const AvailableBaseline> baselines,
    const devicefs::vshadow::SnapshotSet &payload,
    const IncrementalDiagnosticOptions &options) {
    const auto intervals = AssociateSnapshotIntervals(
        baselines, payload);
    if (intervals.empty()) {
        throw std::runtime_error(
            "none of the selected volumes has a retained snapshot "
            "still available");
    }

    const auto unavailable_snapshots =
        payload.snapshots.size() - intervals.size();
    PrintUnavailableSnapshots(unavailable_snapshots);
    auto maps = BuildDirtyBlockReports(
        cancellation_event, intervals);
    auto result = IncrementalDiagnosticResult{
        .map_exit_code = maps.exit_code == kTemporarySnapshotsComplete
            ? 0 : maps.exit_code,
        .reports = std::move(maps.reports),
    };
    if (options.verify) {
        result.verification_exit_code = RunIncrementalVerification(
            cancellation_event, result.reports,
            intervals.size() - result.reports.size(),
            unavailable_snapshots);
    }
    if (options.synthetic_backup_prefix &&
        (result.map_exit_code == 0) &&
        !internal::CancellationRequested(cancellation_event)) {
        static_cast<void>(RunBackupViewDiagnostics(
            cancellation_event, result.reports,
            *options.synthetic_backup_prefix, std::nullopt,
            unavailable_snapshots));
    }
    if (options.filesystem_verification_prefix &&
        (result.map_exit_code == 0) &&
        !internal::CancellationRequested(cancellation_event)) {
        result.filesystem_verification_exit_code =
            RunBackupViewDiagnostics(
                cancellation_event, result.reports,
                *options.filesystem_verification_prefix,
                options.filesystem_verification_percentage,
                unavailable_snapshots);
    }
    return result;
}

[[nodiscard]] auto RunSnapshotDiagnostics(
    const HANDLE cancellation_event,
    const std::span<const std::wstring> volumes,
    const std::span<const AvailableBaseline> baselines,
    const IncrementalDiagnosticOptions &options) {
    auto result = SnapshotDiagnosticResult{};
    result.exit_code = internal::RunVssOperation(
        cancellation_event,
        [&] {
            return devicefs::vshadow::Run(
                cancellation_event, false, volumes,
                [&](const devicefs::vshadow::SnapshotSet &payload) {
                    result.diagnostics = RunPayloadDiagnostics(
                        cancellation_event, baselines, payload, options);
                    return kTemporarySnapshotsComplete;
                });
        });
    return result;
}

} // namespace

export [[nodiscard]] auto RunIncrementalDiagnostics(
    const HANDLE cancellation_event,
    const IncrementalDiagnosticOptions &options) -> int {
    const auto baseline_result = [&]()
        -> std::expected<std::vector<AvailableBaseline>, int> {
        if (options.baseline_snapshot_identifier) {
            return QueryBaselineSnapshot(
                *options.baseline_snapshot_identifier);
        }
        const auto previous = RetrievePreviousBackupManifest(
            cancellation_event, options.namespace_override);
        if (!previous) {
            return std::unexpected{internal::kCancelledExitCode};
        }
        if (previous->exit_code != 0) {
            return std::unexpected{previous->exit_code};
        }
        const auto snapshot_volumes =
            previous->ParseManifest().QuerySnapshotVolumes();
        if (snapshot_volumes.empty()) {
            throw std::runtime_error(
                "the previous backup has no snapshots still available");
        }
        return CollectAvailableBaselines(snapshot_volumes);
    }();
    if (!baseline_result) {
        return baseline_result.error();
    }
    const auto &baselines = *baseline_result;
    if (internal::CancellationRequested(cancellation_event)) {
        return internal::kCancelledExitCode;
    }

    const auto volumes = SelectPayloadVolumes(
        baselines, options.volume_override);
    const auto result = RunSnapshotDiagnostics(
        cancellation_event, volumes, baselines, options);
    if (result.exit_code != kTemporarySnapshotsComplete) {
        return result.exit_code;
    }
    if (options.print_statistics &&
        (result.diagnostics.map_exit_code == 0)) {
        PrintStatistics(result.diagnostics.reports);
    }
    return result.diagnostics.map_exit_code != 0
        ? result.diagnostics.map_exit_code
        : result.diagnostics.verification_exit_code != 0
            ? result.diagnostics.verification_exit_code
            : result.diagnostics.filesystem_verification_exit_code;
}
