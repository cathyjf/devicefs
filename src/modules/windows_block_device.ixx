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

#include <sal.h>
#include <windows.h>
#include <winioctl.h>

// wil uses symbols defined in <algorithm> without including it.
#include <algorithm>

#include <devicefs/winfsp_compat.h>
#include <wil/resource.h>
#include <wil/safecast.h>

#include <cstddef>

export module devicefs.windows_block_device;

import std;
import devicefs.common;
import devicefs.stream_writer;

#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
import devicefs.filesystem_measurement;
#endif

export namespace devicefs {

class SnapshotAllocationBitmap {
  public:
    SnapshotAllocationBitmap(SnapshotAllocationBitmap &&) noexcept;
    auto operator=(SnapshotAllocationBitmap &&) noexcept
        -> SnapshotAllocationBitmap &;
    ~SnapshotAllocationBitmap();

    [[nodiscard]] auto VolumeSize() const noexcept -> std::uint64_t;
    [[nodiscard]] auto ClusterSize() const noexcept -> std::uint32_t;
    auto SynthesizeFreeClusters(
        std::span<unsigned char> output,
        std::uint64_t offset) const noexcept -> void;

  private:
    struct State;

    explicit SnapshotAllocationBitmap(
        std::unique_ptr<State> state) noexcept;

    std::unique_ptr<State> state_;

    friend auto LoadSnapshotAllocationBitmap(
        std::wstring_view,
        std::string_view) -> SnapshotAllocationBitmap;
};

[[nodiscard]] auto LoadSnapshotAllocationBitmap(
    std::wstring_view snapshot,
    std::string_view description) -> SnapshotAllocationBitmap;

struct AllocationChangeBlocks {
    std::uint64_t volume_size = 0;
    // Sorted, unique block starts intersecting an allocation-bit change.
    std::vector<std::uint64_t> block_offsets;
};

[[nodiscard]] auto ReadAllocationChangeBlocks(
    std::wstring_view previous_snapshot,
    std::wstring_view current_snapshot,
    std::uint64_t block_size) -> AllocationChangeBlocks;

} // namespace devicefs

#if defined(__INTELLISENSE__) && !defined(__cpp_lib_start_lifetime_as)
// IntelliSense uses EDG, which does not yet expose `std::start_lifetime_as`.
// Tracked by <https://github.com/microsoft/STL/issues/6169>.
namespace std {
template <class T> auto start_lifetime_as(void *) noexcept -> T *;
} // namespace std
#endif

namespace devicefs::filesystem_internal {

_Success_(return == ERROR_SUCCESS)
[[nodiscard]] auto Ioctl(const HANDLE device, const DWORD code,
    _Out_writes_bytes_opt_(output_size) void *const output,
    const DWORD output_size,
    _In_reads_bytes_opt_(input_size) void *const input,
    const DWORD input_size,
    _Out_opt_ DWORD *const bytes_returned) -> DWORD {
    auto event = wil::unique_event_nothrow{};
    if (!event.try_create(wil::EventOptions::ManualReset, nullptr)) {
        return GetLastError();
    }

    auto operation = OVERLAPPED{.hEvent = event.get()};
    auto returned = DWORD{};
    auto *const result_size = bytes_returned == nullptr ? &returned : bytes_returned;
    if (DeviceIoControl(device, code, input, input_size,
            output, output_size, result_size, &operation)) {
        return DWORD{ERROR_SUCCESS};
    }
    const auto error = GetLastError();
    if (error != ERROR_IO_PENDING) {
        return error;
    }
    if (!GetOverlappedResult(device, &operation, result_size, TRUE)) {
        return GetLastError();
    }
    return DWORD{ERROR_SUCCESS};
}

// This overloaded declaration of `Ioctl` exists to work around an apparent
// defect in MSVC Code Analysis. In a previous version of the code, there
// was only a single declaration of `Ioctl`, and it had two optional pointer
// arguments with default values of `nullptr`. When the former declaration
// was consumed by a module implementation unit, Code Analysis emitted
// C26477 ("Use `nullptr` rather than 0 or NULL"), even though the default
// values were already spelled `nullptr`. Adding a suppression to the
// declaration of `Ioctl` was ineffective to prevent C26477 from being
// raised. Instead, this overloaded version with fewer arguments avoids the
// need for default arguments.
_Success_(return == ERROR_SUCCESS)
[[nodiscard]] auto Ioctl(const HANDLE device, const DWORD code,
    _Out_writes_bytes_opt_(output_size) void *const output,
    const DWORD output_size) -> DWORD {
    return Ioctl(
        device, code, output, output_size, nullptr, 0, nullptr);
}

inline constexpr auto kVolumeBitmapHeaderSize =
    offsetof(VOLUME_BITMAP_BUFFER, Buffer);
inline constexpr auto kBitsPerByte = std::numeric_limits<BYTE>::digits;

struct AllocationBitmap {
    UINT32 cluster_size = 0;
    UINT64 cluster_count = 0;
    wil::unique_virtualalloc_ptr<BYTE> storage;
#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
    std::unique_ptr<FreeClusterMeasurement> measurement;
#endif

    [[nodiscard]] auto IsAllocated(const UINT64 cluster) const noexcept {
        // Preserve any device tail not represented by NTFS clusters.
        if (cluster >= cluster_count) {
            return true;
        }
        return (storage.get()[kVolumeBitmapHeaderSize + cluster / kBitsPerByte] &
            (1u << (cluster % kBitsPerByte))) != 0;
    }

    _Pre_satisfies_((cluster_count == 0) || (cluster_size != 0))
    [[nodiscard]] auto HasAllocatedClusters(
        const UINT64 offset,
        _In_range_(1, MAXUINT64 - offset) const UINT64 length) const noexcept {
        if (!storage) {
            return true;
        }
        _Analysis_assume_(cluster_count != 0);

        const auto first_cluster = offset / cluster_size;
        const auto last_cluster = (offset + (length - 1)) / cluster_size;
        if (last_cluster >= cluster_count) {
            return true;
        }
        for (auto cluster = first_cluster; cluster <= last_cluster; ++cluster) {
            if (IsAllocated(cluster)) {
                return true;
            }
        }
        return false;
    }

    _Pre_satisfies_((cluster_count == 0) || (cluster_size != 0))
    auto SynthesizeFreeClusters(
        const std::span<BYTE> output,
        const UINT64 offset) const noexcept {
        if (!storage || output.empty()) {
            return;
        }
        _Analysis_assume_(cluster_count != 0);

        const auto end = offset + output.size();
        const auto first_cluster = offset / cluster_size;
        const auto last_cluster = (end - 1) / cluster_size;
#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
        measurement->ObserveRead(output, offset);
#endif
        auto free_begin = end;
        for (auto cluster = first_cluster; cluster <= last_cluster; ++cluster) {
            const auto position = std::max(offset, cluster * cluster_size);
            if (!IsAllocated(cluster)) {
                if (free_begin == end) {
                    free_begin = position;
                }
            } else if (free_begin != end) {
                std::ranges::fill(
                    output.subspan(free_begin - offset, position - free_begin), 0);
                free_begin = end;
            }
        }
        if (free_begin != end) {
            std::ranges::fill(
                output.subspan(free_begin - offset, end - free_begin), 0);
        }
    }
};

[[nodiscard]] auto LoadAllocationBitmap(
    HANDLE device, UINT64 device_size,
    std::string_view description) -> AllocationBitmap;

} // namespace devicefs::filesystem_internal

export namespace devicefs {

struct WindowsBlockDevice {
    static constexpr auto kAdvertisedSectorSize = UINT16{512};
    static constexpr auto kMeasureFreeClusterData =
        DEVICEFS_MEASURE_FREE_CLUSTER_DATA != 0;

    const std::uint64_t length;
    std::filesystem::path filename;
    wil::unique_hfile handle;
    UINT32 sector_size = 0;
    filesystem_internal::AllocationBitmap allocation_bitmap;

    [[nodiscard]] static auto FromFilename(
        std::filesystem::path filename, bool extended_dasd,
        bool cache, bool synthetic_free_clusters,
        std::string_view description) -> WindowsBlockDevice;

    template <typename... Observers>
    _Success_(return == STATUS_SUCCESS)
    auto Read(
        _Out_writes_bytes_to_(wanted, transferred) void *const buffer,
        _In_range_(0, length - 1) const std::uint64_t offset,
        _In_range_(1, length - offset) const ULONG wanted,
        _Pre_equal_to_(0) ULONG &transferred,
        Observers &...observers) const noexcept -> NTSTATUS {
        const auto output = std::span<BYTE>{static_cast<BYTE *>(buffer), wanted};
        if constexpr (!kMeasureFreeClusterData) {
            if (!allocation_bitmap.HasAllocatedClusters(offset, wanted)) {
                (observers.RecordSynthetic(), ...);
                std::ranges::fill(output, BYTE{});
                transferred = wanted;
                return STATUS_SUCCESS;
            }
        }
        const auto failure = [&](const DWORD error) {
            devicefs::WriteToStream(
                std::cerr,
                L"devicefs: read failed for '{}': Windows error {}\n",
                filename.native(), error);
            return FspNtStatusFromWin32(error);
        };
        const auto read = [&](void *const output, const UINT64 position,
                              const auto count, auto *const done) {
            // ReadFile resets the event; each dispatcher thread uses it serially.
            thread_local auto event = wil::unique_event_nothrow{};
            if (!event && !event.try_create(wil::EventOptions::ManualReset, nullptr)) {
                return failure(GetLastError());
            }
            auto operation = OVERLAPPED{};
            const auto parts = ULARGE_INTEGER{.QuadPart = position};
            operation.Offset = parts.LowPart;
            operation.OffsetHigh = parts.HighPart;
            operation.hEvent = event.get();
            (observers.BeginSourceRead(), ...);
            // GetOverlappedResult supplies the byte count for either completion path.
            if (!ReadFile(handle.get(), output, count, nullptr, &operation)) {
                const auto error = GetLastError();
                if (error != ERROR_IO_PENDING) {
                    return failure(error);
                }
                (observers.RecordSourcePending(), ...);
            }
            if (!GetOverlappedResult(handle.get(), &operation, done, TRUE)) {
                return failure(GetLastError());
            }
            if (*done != count) {
                return failure(ERROR_READ_FAULT);
            }
            (observers.FinishSourceRead(*done), ...);
            return STATUS_SUCCESS;
        };

        const auto read_offset = offset - (offset % sector_size);
        const auto end = offset + wanted;
        const auto read_end = ((end + sector_size - 1) / sector_size) * sector_size;
        using LengthType = std::remove_cv_t<decltype(wanted)>;
        const auto aligned_length = read_end - read_offset;
        if (!std::in_range<LengthType>(aligned_length)) {
            return STATUS_INVALID_PARAMETER;
        }
        [[gsl::suppress("type.1",
            justification: "std::in_range above proves aligned_length is representable by LengthType.")]]
        const auto read_length = static_cast<LengthType>(aligned_length);
        if ((read_offset == offset) && (read_length == wanted)) {
            const auto status = read(output.data(), offset, wanted, &transferred);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            allocation_bitmap.SynthesizeFreeClusters(output, offset);
            return STATUS_SUCCESS;
        }

        const auto prefix = offset - read_offset;

        auto storage = wil::unique_virtualalloc_ptr<BYTE>(static_cast<BYTE *>(
            VirtualAlloc(nullptr, read_length, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)));
        if (!storage) {
            return failure(GetLastError());
        }
        const auto bounce = std::span<BYTE>{storage.get(), read_length};
        auto device_transferred = LengthType{};
        const auto status = read(
            bounce.data(), read_offset, read_length, &device_transferred);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        transferred = wanted;
        std::ranges::copy(bounce.subspan(prefix, wanted), output.begin());
        allocation_bitmap.SynthesizeFreeClusters(output, offset);
        (observers.RecordBounce(), ...);
        return STATUS_SUCCESS;
    }
};

} // namespace devicefs

namespace devicefs::filesystem_internal {

[[nodiscard]] auto LoadAllocationBitmap(
    const HANDLE device, const UINT64 device_size,
    const std::string_view description) -> AllocationBitmap {
    auto volume = NTFS_VOLUME_DATA_BUFFER{};
    const auto volume_error =
        Ioctl(device, FSCTL_GET_NTFS_VOLUME_DATA, &volume, sizeof(volume));
    if (volume_error != ERROR_SUCCESS) {
        WinError("FSCTL_GET_NTFS_VOLUME_DATA failed for {}", description,
            ExplicitWin32Error{volume_error});
    }
    if ((volume.TotalClusters.QuadPart <= 0) || (volume.BytesPerCluster == 0)) {
        throw std::runtime_error(std::format(
            "FSCTL_GET_NTFS_VOLUME_DATA returned invalid data for {}",
            description));
    }

    // The nonpositive case is rejected above, so this conversion preserves
    // the cluster count.
    const auto cluster_count =
        wil::safe_cast_failfast<UINT64>(volume.TotalClusters.QuadPart);
    if (cluster_count > (device_size / volume.BytesPerCluster)) {
        throw std::runtime_error(std::format(
            "NTFS cluster span exceeds the exposed device length for {}",
            description));
    }

    // The bitmap is applied directly to device offsets, so LCN 0 must begin at byte 0.
    auto retrieval_base = RETRIEVAL_POINTER_BASE{};
    const auto retrieval_base_error = Ioctl(device,
        FSCTL_GET_RETRIEVAL_POINTER_BASE, &retrieval_base, sizeof(retrieval_base));
    if (retrieval_base_error != ERROR_SUCCESS) {
        WinError("FSCTL_GET_RETRIEVAL_POINTER_BASE failed for {}",
            description, ExplicitWin32Error{retrieval_base_error});
    }
    if (retrieval_base.FileAreaOffset.QuadPart != 0) {
        throw std::runtime_error(std::format(
            "NTFS LCN 0 is offset {} sectors from the start of the exposed device "
            "for {}",
            retrieval_base.FileAreaOffset.QuadPart, description));
    }

    const auto bitmap_bytes =
        cluster_count / kBitsPerByte +
        ((cluster_count % kBitsPerByte) != 0);
    const auto bitmap_data_size = kVolumeBitmapHeaderSize + bitmap_bytes;
    const auto output_size = std::max(sizeof(VOLUME_BITMAP_BUFFER), bitmap_data_size);
    if (!std::in_range<DWORD>(output_size)) {
        throw std::runtime_error(std::format(
            "NTFS allocation bitmap is too large for {}", description));
    }
    // std::in_range above proves output_size is representable by DWORD.
    const auto output_size_for_api =
        wil::safe_cast_failfast<DWORD>(output_size);

    auto storage = wil::unique_virtualalloc_ptr<BYTE>(static_cast<BYTE *>(
        VirtualAlloc(nullptr, output_size_for_api, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)));
    if (!storage) {
        WinError(
            "could not allocate NTFS allocation bitmap for {}", description);
    }
    auto *const output = std::start_lifetime_as<VOLUME_BITMAP_BUFFER>(storage.get());
    auto input = STARTING_LCN_INPUT_BUFFER{.StartingLcn = {.QuadPart = 0}};
    auto returned = DWORD{};
    const auto bitmap_error = Ioctl(device, FSCTL_GET_VOLUME_BITMAP,
        output, output_size_for_api, &input, sizeof(input), &returned);
    if (bitmap_error != ERROR_SUCCESS) {
        WinError("FSCTL_GET_VOLUME_BITMAP failed for {}", description,
            ExplicitWin32Error{bitmap_error});
    }
    if ((output->StartingLcn.QuadPart != 0) ||
        (output->BitmapSize.QuadPart != volume.TotalClusters.QuadPart) ||
        (returned < bitmap_data_size)) {
        throw std::runtime_error(std::format(
            "FSCTL_GET_VOLUME_BITMAP returned incomplete data for {}",
            description));
    }

    auto result = AllocationBitmap{
        .cluster_size = volume.BytesPerCluster,
        .cluster_count = cluster_count,
        .storage = std::move(storage),
    };
#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
    result.measurement = std::make_unique<FreeClusterMeasurement>(
        std::span<const BYTE>{
            result.storage.get() + kVolumeBitmapHeaderSize,
            bitmap_bytes,
        },
        volume.BytesPerCluster, cluster_count);
#endif
    return result;
}

} // namespace devicefs::filesystem_internal

namespace devicefs {

using filesystem_internal::AllocationBitmap;
using filesystem_internal::Ioctl;
using filesystem_internal::LoadAllocationBitmap;

auto WindowsBlockDevice::FromFilename(
    std::filesystem::path filename, const bool extended_dasd,
    const bool cache, const bool synthetic_free_clusters,
    const std::string_view description) -> WindowsBlockDevice {
    auto handle = wil::unique_hfile(CreateFileW(filename.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
    if (!handle) {
        WinError("could not open block device for {}", description);
    }

    auto length = GET_LENGTH_INFORMATION{};
    const auto length_error =
        Ioctl(handle.get(), IOCTL_DISK_GET_LENGTH_INFO, &length, sizeof(length));
    if (length_error != ERROR_SUCCESS) {
        WinError("IOCTL_DISK_GET_LENGTH_INFO failed for {}", description,
            ExplicitWin32Error{length_error});
    }
    if (length.Length.QuadPart < 0) {
        throw std::runtime_error(std::format(
            "IOCTL_DISK_GET_LENGTH_INFO returned an invalid length for {}",
            description));
    }

    auto geometry = DISK_GEOMETRY{};
    const auto geometry_error =
        Ioctl(handle.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY, &geometry, sizeof(geometry));
    if (geometry_error != ERROR_SUCCESS) {
        WinError("IOCTL_DISK_GET_DRIVE_GEOMETRY failed for {}", description,
            ExplicitWin32Error{geometry_error});
    }

    // The negative case is rejected above, so this conversion preserves the
    // device length.
    const auto size =
        wil::safe_cast_failfast<UINT64>(length.Length.QuadPart);
    if ((geometry.BytesPerSector == 0) || ((size % geometry.BytesPerSector) != 0)) {
        throw std::runtime_error(std::format(
            "block device length is not a multiple of its sector size for {}",
            description));
    }
    if ((size % kAdvertisedSectorSize) != 0) {
        throw std::runtime_error(std::format(
            "block device length is not a multiple of the advertised "
            "allocation unit for {}",
            description));
    }

    const auto dasd_error = extended_dasd
        ? Ioctl(handle.get(), FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0)
        : DWORD{ERROR_SUCCESS};
    if (dasd_error != ERROR_SUCCESS) {
        const auto error = std::error_code(
            std::bit_cast<int>(dasd_error), std::system_category());
        devicefs::WriteToStream(
            std::cerr,
            L"devicefs: warning: FSCTL_ALLOW_EXTENDED_DASD_IO failed for '{}': ",
            filename.native());
        devicefs::WriteToStream(std::cerr, "{}\n", error.message());
    }

    if (cache || synthetic_free_clusters) {
        auto file_system_flags = DWORD{};
        if (!GetVolumeInformationByHandleW(handle.get(), nullptr, 0, nullptr,
                nullptr, &file_system_flags, nullptr, 0)) {
            WinError("could not query filesystem flags for {}", description);
        }
        if ((file_system_flags & FILE_READ_ONLY_VOLUME) == 0) {
            const auto option = cache ? "--cache" : "--synthetic-free-clusters";
            throw std::runtime_error(std::format(
                "{} requires a read-only volume for {}", option, description));
        }
    }
    auto allocation_bitmap = synthetic_free_clusters
        ? LoadAllocationBitmap(handle.get(), size, description)
        : AllocationBitmap{};
    return WindowsBlockDevice{
        .length = size,
        .filename = std::move(filename),
        .handle = std::move(handle),
        .sector_size = geometry.BytesPerSector,
        .allocation_bitmap = std::move(allocation_bitmap),
    };
}

} // namespace devicefs
