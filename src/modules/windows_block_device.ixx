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

#undef stderr
#undef stdout

export module devicefs.windows_block_device;

import std;
import <cstddef>;
import devicefs.allocation;
import <devicefs/common.h>;
import devicefs.stream_writer;

#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
import devicefs.filesystem_measurement;
#endif

import devicefs.terminal.transcoding;

using devicefs::terminal::Transcode;

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
        std::string_view,
        std::string_view) -> SnapshotAllocationBitmap;
};

[[nodiscard]] auto LoadSnapshotAllocationBitmap(
    std::string_view snapshot,
    std::string_view description) -> SnapshotAllocationBitmap;

struct AllocationChangeBlocks {
    std::uint64_t volume_size = 0;
    // Sorted, unique block starts intersecting an allocation-bit change.
    std::vector<std::uint64_t> block_offsets;
};

[[nodiscard]] auto ReadAllocationChangeBlocks(
    std::string_view previous_snapshot,
    std::string_view current_snapshot,
    std::uint64_t block_size) -> AllocationChangeBlocks;

} // namespace devicefs

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
    int cluster_shift = 0;
    UINT64 cluster_count = 0;
    std::unique_ptr<BYTE[]> storage;
#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
    std::unique_ptr<FreeClusterMeasurement> measurement;
#endif

    // Search the cluster indices in [first, limit), returning the first match or
    // `limit` if none exists. If `allocated` is true, search for an allocated
    // cluster (a set bit); otherwise, search for a free cluster (a clear bit).
    // The range must lie within the bitmap. Each iteration skips a whole word
    // if it contains no matching bits.
    template <bool allocated>
    _Pre_satisfies_((first <= limit) && (limit <= cluster_count))
    _Post_satisfies_((first <= return) && (return <= limit))
    [[nodiscard]] auto FindNextCluster(const UINT64 first, const UINT64 limit) const noexcept {
        if (first == limit) {
            return limit;
        }
        constexpr auto bits_per_word = std::numeric_limits<UINT64>::digits;
        const auto bytes = std::span{storage.get() + kVolumeBitmapHeaderSize,
            cluster_count / kBitsPerByte + ((cluster_count % kBitsPerByte) != 0)};
        const auto first_bit = first % bits_per_word;
        // Only the first word can begin before `first`. Its mask excludes those
        // earlier cluster indices; subsequent words need no such mask.
        auto mask = MAXUINT64 << first_bit;
        for (auto word_begin = first - first_bit; word_begin < limit;
             word_begin += bits_per_word) {
            const auto remaining = bytes.subspan(word_begin / kBitsPerByte);
            auto word = UINT64{};
            // MSVC 19.52 on ARM64 calls `memcpy` on every iteration when a
            // ternary selects the length. Keeping the constant-size copy in its
            // own branch lets the compiler use one load for each complete word.
            if (remaining.size() >= sizeof(word)) {
                std::memcpy(&word, remaining.data(), sizeof(word));
            } else {
                std::memcpy(&word, remaining.data(), remaining.size());
            }
            // `VOLUME_BITMAP_BUFFER` numbers bits from the low bit of each byte.
            // Reversing the bytes on a big-endian host preserves that order within
            // the word.
            if constexpr (std::endian::native == std::endian::big) {
                word = std::byteswap(word);
            }
            const auto matching = (allocated ? word : ~word) & mask;
            if (matching != 0) {
                return std::min(limit, word_begin + std::countr_zero(matching));
            }
            mask = MAXUINT64;
        }
        return limit;
    }

    _Pre_satisfies_((cluster_count == 0) || (cluster_size != 0))
    [[nodiscard]] auto HasAllocatedClusters(
        const UINT64 offset,
        _In_range_(1, MAXUINT64 - offset) const UINT64 length) const noexcept {
        if (!storage) {
            return true;
        }

        const auto first_cluster = offset >> cluster_shift;
        const auto last_cluster =
            (offset + (length - 1)) >> cluster_shift;
        if (last_cluster >= cluster_count) {
            return true;
        }
        return FindNextCluster<true>(first_cluster, last_cluster + 1) <= last_cluster;
    }

    _Pre_satisfies_((cluster_count == 0) || (cluster_size != 0))
    auto SynthesizeFreeClusters(
        const std::span<BYTE> output,
        const UINT64 offset) const noexcept {
        if (!storage || output.empty()) {
            return;
        }

        const auto end = offset + output.size();
        const auto first_cluster = offset >> cluster_shift;
        // Bytes beyond the volume's cluster count must retain their source contents.
        const auto limit = std::min(cluster_count, ((end - 1) >> cluster_shift) + 1);
#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
        measurement->ObserveRead(output, offset);
#endif
        auto cluster = first_cluster;
        while (cluster < limit) {
            const auto free_cluster = FindNextCluster<false>(cluster, limit);
            if (free_cluster == limit) {
                return;
            }
            cluster = FindNextCluster<true>(free_cluster, limit);
            const auto free_begin = std::max(offset, free_cluster << cluster_shift);
            const auto free_end = std::min(end, cluster << cluster_shift);
            const auto free = output.subspan(free_begin - offset, free_end - free_begin);
            // MSVC 19.52's `std::ranges::fill` misses its `memset` optimization
            // and emits a byte-at-a-time loop. `std::fill` reaches `memset`,
            // so we use it for the zero-filling operations in this file.
            std::fill(free.begin(), free.end(), 0);
        }
    }
};

[[nodiscard]] auto LoadAllocationBitmap(
    HANDLE device,
    const std::filesystem::path &filename,
    std::string_view description) -> AllocationBitmap;

} // namespace devicefs::filesystem_internal

export namespace devicefs {

struct WindowsBlockDevice {
    // A thread reuses its retained read buffer across devices, so that buffer
    // must satisfy every device's alignment requirement. The default is the
    // system page size; device initialization replaces it with the maximum
    // requirement before any reads begin. It remains unchanged thereafter
    // because buffers allocated by earlier reads retain their original alignment.
    inline static auto read_buffer_alignment = [] {
        auto information = SYSTEM_INFO{};
        GetSystemInfo(&information);
        return std::align_val_t{information.dwPageSize};
    }();
    static constexpr auto kAdvertisedSectorSize = UINT16{512};
    static constexpr auto kMeasureFreeClusterData =
        DEVICEFS_MEASURE_FREE_CLUSTER_DATA != 0;

    const std::uint64_t length;
    std::filesystem::path filename;
    wil::unique_hfile handle;
    const std::align_val_t buffer_alignment = std::align_val_t{1};
    UINT32 sector_size = 0;
    filesystem_internal::AllocationBitmap allocation_bitmap;

    [[nodiscard]] static auto FromFilename(
        std::filesystem::path filename, bool extended_dasd,
        bool cache, bool synthetic_free_clusters,
        std::string_view description) -> WindowsBlockDevice;

    // Return a bitmap that lets a backup client skip reading chunks that `Read`
    // fills entirely with zeros. Each chunk contains `chunk_size` bytes, except
    // that the last chunk ends at the end of the device.
    // A bit value of 0 means the client can supply zeros for that chunk;
    // a bit value of 1 means the client must read the chunk from the image.
    // The first eight chunks use bits 0 through 7 of the first byte, where
    // bit 0 is the least significant bit. The next eight use the second byte.
    [[nodiscard]] auto MakeKnownDataMap(const std::uint64_t chunk_size) const
        -> std::vector<unsigned char> {
        if (chunk_size == 0) {
            throw std::invalid_argument("known-data map chunk size must be nonzero");
        }
        constexpr auto bits_per_byte = filesystem_internal::kBitsPerByte;
        const auto chunks = length / chunk_size + ((length % chunk_size) != 0);
        const auto bytes = chunks / bits_per_byte + ((chunks % bits_per_byte) != 0);
        auto result = std::vector<unsigned char>(
            wil::safe_cast_failfast<std::size_t>(bytes));
        for (auto chunk = std::uint64_t{}; chunk < chunks; ++chunk) {
            const auto offset = chunk * chunk_size;
            // For a nonempty device, the last chunk index is `(length - 1) / chunk_size`.
            // Multiplying that index, or any earlier index, by `chunk_size` gives an
            // `offset` of at most `length - 1`. An empty device never enters the loop.
            _Analysis_assume_(length > offset);
            const auto count = std::min(chunk_size, length - offset);
            // Both arguments to `std::min` are positive: zero `chunk_size` was
            // rejected above, and `offset` is below `length`. Their minimum is
            // therefore positive and cannot exceed the remaining device length.
            _Analysis_assume_((count > 0) && (count <= (length - offset)));
            // Because `length` is a `uint64_t`, its value cannot exceed `MAXUINT64`.
            // Further, `offset` is less than `length` and is also a `uint64_t`, so
            // `length - offset` cannot underflow.
            // Thus, `count <= (length - offset)` implies `count <= (MAXUINT64 - offset)`.
            _Analysis_assume_(count <= (MAXUINT64 - offset));
            if (allocation_bitmap.HasAllocatedClusters(offset, count)) {
                result.at(chunk / bits_per_byte) |=
                    wil::safe_cast_failfast<unsigned char>(1u << (chunk % bits_per_byte));
            }
        }
        return result;
    }

    template <typename... Observers>
    [[gsl::suppress("26445",
        justification:
            "This function contains a structured binding that copies a pair, "
            "including a `std::span`. C26445 incorrectly diagnoses a reference "
            "to the `std::span` even though the declaration uses `const auto`, "
            "without `&`. A suppression placed closer to the structured binding "
            "was ineffective.")]]
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
                std::fill(output.begin(), output.end(), BYTE{});
                transferred = wanted;
                return STATUS_SUCCESS;
            }
        }
        const auto failure = [&](const DWORD error) {
            devicefs::WriteToStream(
                devicefs::stderr,
                L"devicefs: read failed for '{}' at offset 0x{:x} "
                L"for {} bytes: Windows error {}\n",
                std::wstring_view{filename.native()}, offset, wanted, error);
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

        // Windows documents sector sizes as powers of two, so these masks round
        // to sector boundaries without division on each read.
        // https://learn.microsoft.com/en-us/windows/win32/fileio/file-buffering#alignment-and-file-access-requirements
        const auto sector_mask = CompileTimeCast<UINT64>(sector_size) - 1;
        const auto read_offset = offset & ~sector_mask;
        const auto end = offset + wanted;
        const auto read_end = (end + sector_mask) & ~sector_mask;
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

        const auto [bounce, temporary_backing_memory] = [read_length]
            -> std::pair<std::span<BYTE>, decltype(NewAlignedArray(read_length, read_buffer_alignment))> {
            // This 5 MIB capacity comfortably accommodates a 4 MiB PBS chunk
            // and any extra bytes needed for sector alignment.
            constexpr auto kRetainedReadBufferSize = 5uz * 1024 * 1024;
            if (read_length <= kRetainedReadBufferSize) {
                thread_local const auto retained_buffer_storage =
                    NewAlignedArray(kRetainedReadBufferSize, read_buffer_alignment);
                return {std::span{retained_buffer_storage.get(), read_length},
                    {nullptr, retained_buffer_storage.get_deleter()}};
            }
            auto newly_allocated_memory = NewAlignedArray(read_length, read_buffer_alignment);
            return {std::span{newly_allocated_memory.get(), read_length},
                std::move(newly_allocated_memory)};
        }();
        auto device_transferred = LengthType{};
        const auto status = read(
            bounce.data(), read_offset, read_length, &device_transferred);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        transferred = wanted;
        std::ranges::copy(bounce.subspan(offset - read_offset, wanted), output.begin());
        allocation_bitmap.SynthesizeFreeClusters(output, offset);
        (observers.RecordBounce(), ...);
        return STATUS_SUCCESS;
    }
};

// A filesystem exposing known-data maps contains both volume images and bitmap
// files. This device type serves those two kinds of file; ordinary mounts use
// `WindowsBlockDevice` directly.
struct WindowsDeviceOrBitmap {
    const std::uint64_t length;
    std::variant<WindowsBlockDevice, std::vector<BYTE>> contents;

    [[nodiscard]] static auto FromBlockDevice(WindowsBlockDevice device) noexcept {
        return WindowsDeviceOrBitmap{
            .length = device.length,
            .contents = std::move(device),
        };
    }

    template <typename... Observers>
    _Success_(return == STATUS_SUCCESS)
    auto Read(
        _Out_writes_bytes_to_(wanted, transferred) void *const buffer,
        _In_range_(0, length - 1) const std::uint64_t offset,
        _In_range_(1, length - offset) const ULONG wanted,
        _Pre_equal_to_(0) ULONG &transferred,
        Observers &...observers) const noexcept -> NTSTATUS {
        if (const auto *const device = std::get_if<WindowsBlockDevice>(&contents)) [[likely]] {
            [[msvc::forceinline_calls]]
            return device->Read(
                buffer, offset, wanted, transferred, observers...);
        }
        const auto &bitmap = std::get<std::vector<BYTE>>(contents);
        std::ranges::copy(std::span{bitmap}.subspan(offset, wanted),
            std::span{static_cast<BYTE *>(buffer), wanted}.begin());
        transferred = wanted;
        return STATUS_SUCCESS;
    }
};

} // namespace devicefs

namespace devicefs::filesystem_internal {

[[nodiscard]] auto LoadAllocationBitmap(
    const HANDLE device,
    const std::filesystem::path &filename,
    const std::string_view description) -> AllocationBitmap {
    const auto file_system = [device, &filename, description] {
        auto file_system = std::array<wchar_t, MAX_PATH + 1>{};
        if (!GetVolumeInformationByHandleW(device, nullptr, 0, nullptr, nullptr,
            nullptr, file_system.data(),
            CompileTimeCast<DWORD, std::size(file_system)>())) {
            WinError("could not identify the filesystem for '{}' ({})",
                std::wstring_view{filename.native()}, description);
        }
        return file_system;
    }();
    // `GetDiskSpaceInformationW` returns `STATUS_INVALID_PARAMETER` for the
    // read-only ReFS test volumes. The filesystem-specific control codes
    // successfully return their cluster counts and sizes through the volume handle.
    const auto query_geometry = [device, &filename,
        file_system_ = std::wstring_view{file_system.data()}, description](
        auto volume, const DWORD code) {
        const auto error = Ioctl(device, code, &volume, sizeof(volume));
        if (error != ERROR_SUCCESS) {
            WinError("querying the {} volume data failed for '{}' ({})",
                file_system_, std::wstring_view{filename.native()}, description,
                ExplicitWin32Error{error});
        }
        return std::pair{
            wil::safe_cast_failfast<UINT64>(volume.TotalClusters.QuadPart),
            volume.BytesPerCluster};
    };
    const auto [cluster_count, cluster_size] =
        (std::wstring_view{file_system.data()} == L"ReFS")
        ? query_geometry(REFS_VOLUME_DATA_BUFFER{}, FSCTL_GET_REFS_VOLUME_DATA)
        : query_geometry(NTFS_VOLUME_DATA_BUFFER{}, FSCTL_GET_NTFS_VOLUME_DATA);

    // The bitmap is applied directly to device offsets, so LCN 0 must begin at byte 0.
    {
        auto retrieval_base = RETRIEVAL_POINTER_BASE{};
        const auto retrieval_base_error = Ioctl(device,
            FSCTL_GET_RETRIEVAL_POINTER_BASE, &retrieval_base, sizeof(retrieval_base));
        if (retrieval_base_error != ERROR_SUCCESS) {
            WinError("FSCTL_GET_RETRIEVAL_POINTER_BASE failed for '{}' ({})",
                std::wstring_view{filename.native()}, description,
                ExplicitWin32Error{retrieval_base_error});
        }
        if (retrieval_base.FileAreaOffset.QuadPart != 0) {
            throw std::runtime_error(std::format(
                "LCN 0 is offset {} sectors from the start of the exposed device "
                "'{}' ({})",
                retrieval_base.FileAreaOffset.QuadPart,
                Transcode<std::string>(filename.native()), description));
        }
    }

    const auto bitmap_bytes =
        cluster_count / kBitsPerByte +
        ((cluster_count % kBitsPerByte) != 0);
    const auto bitmap_data_size = kVolumeBitmapHeaderSize + bitmap_bytes;
    const auto output_size_for_api =
        [output_size = std::max(sizeof(VOLUME_BITMAP_BUFFER), bitmap_data_size),
        &filename, description] {
        auto converted = DWORD{};
        if (FAILED(wil::safe_cast_nothrow(output_size, &converted))) {
            throw std::runtime_error(std::format(
                "the allocation bitmap for '{}' requires {} bytes, exceeding "
                "the DeviceIoControl buffer limit of {} bytes ({})",
                Transcode<std::string>(filename.native()), output_size,
                std::numeric_limits<DWORD>::max(), description));
        }
        return converted;
    }();

    auto storage = std::make_unique_for_overwrite<BYTE[]>(output_size_for_api);
    [[gsl::suppress("26403",
        justification:
            "The pointer `output` is not the owner of the memory to which it "
            "points. The owner is `storage`, which is a smart pointer.")]]
    auto *const output = ::new (storage.get()) VOLUME_BITMAP_BUFFER;
    auto input = STARTING_LCN_INPUT_BUFFER{.StartingLcn = {.QuadPart = 0}};
    auto returned = DWORD{};
    const auto bitmap_error = Ioctl(device, FSCTL_GET_VOLUME_BITMAP,
        output, output_size_for_api, &input, sizeof(input), &returned);
    // Only the bitmap prefix covering this image is needed. `ERROR_MORE_DATA`
    // is acceptable when that entire prefix was returned; additional bits
    // describe clusters beyond the volume geometry and are never consulted.
    if ((bitmap_error != ERROR_SUCCESS) && (bitmap_error != ERROR_MORE_DATA)) {
        WinError("FSCTL_GET_VOLUME_BITMAP failed for '{}' ({})",
            std::wstring_view{filename.native()}, description,
            ExplicitWin32Error{bitmap_error});
    }
    if ((returned < bitmap_data_size) || (output->StartingLcn.QuadPart != 0) ||
        std::cmp_less(output->BitmapSize.QuadPart, cluster_count)) {
        throw std::runtime_error(std::format(
            "FSCTL_GET_VOLUME_BITMAP returned incomplete data for '{}' ({})",
            Transcode<std::string>(filename.native()), description));
    }

    auto result = AllocationBitmap{
        .cluster_size = cluster_size,
        .cluster_shift = std::countr_zero(cluster_size),
        .cluster_count = cluster_count,
        .storage = std::move(storage),
    };
#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
    result.measurement = std::make_unique<FreeClusterMeasurement>(
        std::span<const BYTE>{
            result.storage.get() + kVolumeBitmapHeaderSize,
            bitmap_bytes,
        },
        cluster_size, cluster_count);
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
        WinError("could not open block device '{}' for {}",
            std::wstring_view{filename.native()}, description);
    }

    const auto alignment = QueryBufferAlignment(handle.get());
    if (!alignment) {
        WinError("could not query buffer alignment for block device '{}' ({})",
            std::wstring_view{filename.native()}, description);
    }

    auto length = GET_LENGTH_INFORMATION{};
    const auto length_error =
        Ioctl(handle.get(), IOCTL_DISK_GET_LENGTH_INFO, &length, sizeof(length));
    if (length_error != ERROR_SUCCESS) {
        WinError("IOCTL_DISK_GET_LENGTH_INFO failed for '{}' ({})",
            std::wstring_view{filename.native()}, description,
            ExplicitWin32Error{length_error});
    }

    auto geometry = DISK_GEOMETRY{};
    const auto geometry_error =
        Ioctl(handle.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY, &geometry, sizeof(geometry));
    if (geometry_error != ERROR_SUCCESS) {
        WinError("IOCTL_DISK_GET_DRIVE_GEOMETRY failed for '{}' ({})",
            std::wstring_view{filename.native()}, description,
            ExplicitWin32Error{geometry_error});
    }

    const auto size =
        wil::safe_cast_failfast<UINT64>(length.Length.QuadPart);
    if ((size % kAdvertisedSectorSize) != 0) {
        throw std::runtime_error(std::format(
            "block device '{}' has a {}-byte length that is not a multiple of "
            "the advertised {}-byte allocation unit ({})",
            Transcode<std::string>(filename.native()), size, kAdvertisedSectorSize, description));
    }

    const auto dasd_error = extended_dasd
        ? Ioctl(handle.get(), FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0)
        : DWORD{ERROR_SUCCESS};
    if (dasd_error != ERROR_SUCCESS) {
        const auto error = std::error_code(
            std::bit_cast<int>(dasd_error), std::system_category());
        devicefs::WriteToStream(
            devicefs::stderr,
            L"devicefs: warning: FSCTL_ALLOW_EXTENDED_DASD_IO failed for '{}': ",
            filename.native());
        devicefs::WriteToStream(devicefs::stderr, "{}\n", error.message());
    }

    if (cache || synthetic_free_clusters) {
        auto file_system_flags = DWORD{};
        if (!GetVolumeInformationByHandleW(handle.get(), nullptr, 0, nullptr,
                nullptr, &file_system_flags, nullptr, 0)) {
            WinError("could not query filesystem flags for '{}' ({})",
                std::wstring_view{filename.native()}, description);
        }
        if ((file_system_flags & FILE_READ_ONLY_VOLUME) == 0) {
            const auto option = cache ? "--cache" : "--synthetic-free-clusters";
            throw std::runtime_error(std::format(
                "{} requires read-only block device '{}' ({})",
                option, Transcode<std::string>(filename.native()), description));
        }
    }
    auto allocation_bitmap = synthetic_free_clusters
        ? LoadAllocationBitmap(handle.get(), filename, description)
        : AllocationBitmap{};
    return WindowsBlockDevice{
        .length = size,
        .filename = std::move(filename),
        .handle = std::move(handle),
        .buffer_alignment = *alignment,
        .sector_size = geometry.BytesPerSector,
        .allocation_bitmap = std::move(allocation_bitmap),
    };
}

} // namespace devicefs
