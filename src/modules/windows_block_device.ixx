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

export module devicefs.windows_block_device;

import std;
import <devicefs/windows_imports.h>;
import devicefs.allocation;
import <devicefs/common.h>;
import <devicefs/malloca.h>;
import devicefs.stream_writer;

#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
import devicefs.filesystem_measurement;
#endif

import devicefs.terminal.transcoding;

using devicefs::terminal::Transcode;

export namespace devicefs {

constexpr auto kHandleDevicePrefix = std::string_view{R"(\\\handle:)"};

// On arm64, MSVC 14.51's implementation of `std::countr_zero` uses a software
// loop instead of the `RBIT` and `CLZ` instructions emitted by this intrinsic.
[[nodiscard, msvc::forceinline]]
auto CountTrailingZeros(_Pre_satisfies_(value != 0) const std::uint64_t value)
    noexcept -> int {
    /* [[indeterminate]] */ unsigned long bit_index;
    std::ignore = _BitScanForward64(&bit_index, value);
    return bit_index;
}

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

[[gsl::suppress("26497", justification:
    "Both loading branches call `std::memcpy`, so this function "
    "cannot be evaluated in a constant expression.")]]
[[nodiscard, msvc::forceinline]]
auto LoadBitmapWord(const std::span<const std::byte> bytes) noexcept {
    auto word = [bytes] {
        using Word = std::uint64_t;
        // MSVC 19.52 on ARM64 calls `memcpy` on every iteration when a
        // ternary selects the length. Keeping the constant-size copy in its
        // own branch lets the compiler use one load for each complete word.
        if (bytes.size() >= sizeof(Word)) {
            /* [[indeterminate]] */ Word result;
            std::memcpy(&result, bytes.data(), sizeof(result));
            return result;
        }
        auto result = Word{};
        std::memcpy(&result, bytes.data(), bytes.size());
        return result;
    }();
    // `VOLUME_BITMAP_BUFFER` numbers bits from the low bit of each byte.
    // Reversing the bytes on a big-endian host preserves that order within
    // the word.
    if constexpr (std::endian::native == std::endian::big) {
        word = std::byteswap(word);
    }
    return word;
}

struct AllocationBitmap {
    UINT32 cluster_size = 0;
    int cluster_shift = 0;
    UINT64 cluster_count = 0;
    std::unique_ptr<BYTE[]> storage;
#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
    std::unique_ptr<FreeClusterMeasurement> measurement;
#endif

    [[nodiscard, msvc::forceinline]]
    _Pre_satisfies_(cluster_size != 0)
    constexpr auto BitmapData() const noexcept -> std::span<const BYTE> {
        // The bitmap loader and test fixture set `cluster_size` together
        // with an allocated `storage` buffer. The nonzero `cluster_size`
        // precondition identifies that initialized state, so `storage`
        // is non-null.
        [[assume(storage.get() != nullptr)]];
        return {storage.get() + kVolumeBitmapHeaderSize,
            cluster_count / kBitsPerByte +
                ((cluster_count % kBitsPerByte) != 0)};
    }

    // Search the nonempty range [first, limit) for an allocated cluster (a
    // set bit) or a free cluster (a clear bit). `bytes` begins at cluster zero
    // and covers the searched range. A `UINT64` result is the first matching
    // index, or `limit` if none exists. A `bool` result reports whether a match
    // exists; it can return as soon as a word or block comparison establishes
    // that.
    template <bool allocated, class Result>
        requires (std::same_as<Result, UINT64> || std::same_as<Result, bool>)
    [[nodiscard, msvc::forceinline]]
    _Pre_satisfies_(first < limit)
    static auto FindCluster(const std::span<const BYTE> bytes,
        const UINT64 first, const UINT64 limit) noexcept -> Result {
        // The caller supplies `bytes` covering the searched cluster range,
        // so the byte containing cluster `limit - 1` is within the span.
        _Analysis_assume_(
            ((limit - 1) / kBitsPerByte) < bytes.size());
        const auto no_match = [limit] -> Result {
            if constexpr (std::same_as<Result, bool>) {
                std::ignore = limit;
                return false;
            } else {
                return limit;
            }
        }();
        const auto found = [](const UINT64 word_begin,
            const UINT64 matching) static -> Result {
            if constexpr (std::same_as<Result, bool>) {
                std::ignore = word_begin;
                std::ignore = matching;
                return true;
            } else {
                return word_begin + CountTrailingZeros(matching);
            }
        };
        constexpr auto bits_per_word = std::numeric_limits<UINT64>::digits;
        constexpr auto nonmatching_word = allocated ? UINT64{} : MAXUINT64;
        const auto first_bit = first % bits_per_word;
        const auto first_word = first - first_bit;
        const auto first_matching =
            (LoadBitmapWord(std::as_bytes(
                bytes.subspan(first_word / kBitsPerByte))) ^
                nonmatching_word) & (MAXUINT64 << first_bit);
        // A range ending in this word also needs its upper bits masked out.
        // Longer ranges use only the starting-bit mask here; their final word
        // is handled after the complete-word scan.
        if ((limit - first) <= (bits_per_word - first_bit)) {
            const auto matching = first_matching &
                (MAXUINT64 >> (bits_per_word - (limit - first_word)));
            return matching != 0 ? found(first_word, matching) : no_match;
        }
        if (first_matching != 0) {
            return found(first_word, first_matching);
        }

        std::conditional_t<std::same_as<Result, bool>, const UINT64, UINT64>
            word_begin = first_word + bits_per_word;
        const auto word_limit = limit - (limit % bits_per_word);
        const auto complete_words = bytes.subspan(word_begin / kBitsPerByte,
            (word_limit - word_begin) / kBitsPerByte);
        const auto *cursor = complete_words.data();
        if constexpr (std::same_as<Result, bool>) {
            // PBS reads are usually about 2 MiB, but intervening layers may combine
            // requests. A 128-byte reference lets the complete-word bitmap scan for a
            // 4 MiB read use one `memcmp` call at the common 4 KiB cluster size.
            // Shorter scans compare only the required prefix, so the extra capacity
            // adds no comparison work to those reads.
            //
            // This capacity is chosen for the expected workload. NTFS permits clusters
            // as small as 512 bytes; their longer bitmap scans reuse the reference
            // over multiple comparisons.
            alignas(std::hardware_constructive_interference_size)
            static constexpr auto nonmatching_bytes = [] {
                auto result = std::array<BYTE, 128>{};
                result.fill(allocated ? BYTE{} :
                    std::numeric_limits<BYTE>::max());
                return result;
            }();
            auto remaining = complete_words.size();
            while (remaining != 0) {
                const auto count = std::min(
                    remaining, nonmatching_bytes.size());
                if (std::memcmp(cursor, nonmatching_bytes.data(), count) != 0) {
                    return true;
                }
                cursor += count;
                remaining -= count;
            }
        } else {
            const auto *const word_end = cursor + complete_words.size();
            for (; cursor != word_end;
                 cursor += sizeof(UINT64), word_begin += bits_per_word) {
                const auto matching = LoadBitmapWord(std::as_bytes(
                    std::span<const BYTE, sizeof(UINT64)>{
                        cursor, sizeof(UINT64)})) ^ nonmatching_word;
                if (matching != 0) {
                    return found(word_begin, matching);
                }
            }
        }
        if (word_limit != limit) {
            const auto mask =
                MAXUINT64 >> (bits_per_word - (limit - word_limit));
            const auto matching =
                (LoadBitmapWord(std::as_bytes(
                    bytes.subspan(word_limit / kBitsPerByte))) ^
                    nonmatching_word) & mask;
            if (matching != 0) {
                return found(word_limit, matching);
            }
        }
        return no_match;
    }

    // Search the cluster indices in [first, limit), returning the first match
    // or `limit` if none exists. If `allocated` is true, search for an
    // allocated cluster (a set bit); otherwise, search for a free cluster
    // (a clear bit).
    template <bool allocated>
    _Pre_satisfies_((first <= limit) && (limit <= cluster_count))
    _Post_satisfies_((first <= return) && (return <= limit))
    [[nodiscard]] auto FindNextCluster(
        const UINT64 first, const UINT64 limit) const noexcept {
        if (first == limit) {
            return limit;
        }
        return FindCluster<allocated, UINT64>(BitmapData(), first, limit);
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
        return FindCluster<true, bool>(
            BitmapData(), first_cluster, last_cluster + 1);
    }

    // Precondition: the caller supplies a buffer that does not overlap
    // `*this` or the bitmap storage owned by `storage`. `__restrict` lets
    // the compiler rely on that promise when optimizing buffer writes.
    _Pre_satisfies_((cluster_count == 0) || (cluster_size != 0))
    auto SynthesizeFreeClusters(
        _When_(output_size != 0,
            _Out_writes_bytes_(output_size)
            _When_(DEVICEFS_MEASURE_FREE_CLUSTER_DATA, _Inout_))
        BYTE *__restrict const output,
        const std::size_t output_size,
        const UINT64 offset) const noexcept {
        if (!storage || (output_size == 0)) {
            return;
        }

        const auto bytes = BitmapData();
        const auto shift = cluster_shift;
        const auto end = offset + output_size;
        const auto first_cluster = offset >> shift;
        // Bytes beyond the volume's cluster count must retain their source contents.
        const auto limit = std::min(cluster_count, ((end - 1) >> shift) + 1);
#if DEVICEFS_MEASURE_FREE_CLUSTER_DATA
        measurement->ObserveRead(std::span{output, output_size}, offset);
#endif
        auto cluster = first_cluster;
        while (cluster < limit) {
            const auto free_cluster =
                FindCluster<false, UINT64>(bytes, cluster, limit);
            if (free_cluster == limit) {
                return;
            }
            cluster = FindCluster<true, UINT64>(bytes, free_cluster, limit);
            const auto free_begin = std::max(offset, free_cluster << shift);
            const auto free_end = std::min(end, cluster << shift);
            std::memset(output + (free_begin - offset), 0,
                free_end - free_begin);
        }
    }
};

[[nodiscard]] auto LoadAllocationBitmap(
    HANDLE device,
    const std::filesystem::path &filename,
    std::string_view description) -> AllocationBitmap;

} // namespace devicefs::filesystem_internal

export namespace devicefs {

[[nodiscard]] auto OpenBlockDevice(
    const std::filesystem::path &filename,
    std::string_view description,
    bool inherit_handle = false) -> wil::unique_hfile;

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
        std::filesystem::path filename, auto &&...arguments)
        -> WindowsBlockDevice {
        auto handle = OpenBlockDevice(filename,
            std::get<sizeof...(arguments) - 1>(
                std::forward_as_tuple(arguments...)));
        return FromHandle(std::move(handle), std::move(filename),
            std::forward<decltype(arguments)>(arguments)...);
    }

    // Take ownership of an overlapped read handle. `filename` identifies the
    // source in diagnostics.
    [[nodiscard]] static auto FromHandle(
        wil::unique_hfile handle, std::filesystem::path filename,
        bool extended_dasd, bool cache, bool synthetic_free_clusters,
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
                // MSVC 19.52's `std::ranges::fill` misses its `memset`
                // optimization and emits a byte-at-a-time loop.
                // `std::fill` reaches `memset`, so we use it here.
                std::fill(output.begin(), output.end(), BYTE{});
                transferred = wanted;
                return STATUS_SUCCESS;
            }
        }
        // Read failures are not expected during normal operation. `noinline`
        // prevents `flatten` from enlarging the read callback with diagnostic
        // code that successful reads never execute.
        const auto failure = [&filename = filename, offset, wanted]
            [[msvc::noinline]] (const DWORD error) noexcept {
            devicefs::WriteToStream(
                devicefs::stderr,
                L"devicefs: read failed for '{}' at offset 0x{:x} "
                L"for {} bytes: {}\n",
                std::wstring_view{filename.native()}, offset, wanted,
                TryConstructWinError("", ExplicitWin32Error{error}));
            return FspNtStatusFromWin32(error);
        };
        const auto read = [&](void *const output, const UINT64 position,
            const auto count, auto *const done) {
            // This function does not call itself, so the event can reside
            // in `thread_local` storage.
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
        if (!std::in_range<LengthType>(read_end - read_offset)) {
            return STATUS_INVALID_PARAMETER;
        }
        [[gsl::suppress("26472", justification:
            "This cast is proved safe by the range check above.")]]
        const auto read_length = static_cast<LengthType>(read_end - read_offset);
        if ((read_offset == offset) && (read_length == wanted)) {
            const auto status = read(output.data(), offset, wanted, &transferred);
            if (!NT_SUCCESS(status)) {
                return status;
            }
            allocation_bitmap.SynthesizeFreeClusters(
                output.data(), output.size(), offset);
            return STATUS_SUCCESS;
        }

        const auto read_into_bounce_buffer_and_copy_to_destination =
            [&](BYTE *const bounce_storage) {
            const auto bounce = std::span{bounce_storage, read_length};
            auto device_transferred = LengthType{};
            const auto status = read(
                bounce.data(), read_offset, read_length, &device_transferred);
            if (!NT_SUCCESS(status)) {
                return status;
            }

            transferred = wanted;
            std::ranges::copy(bounce.subspan(offset - read_offset, wanted),
                output.begin());
            allocation_bitmap.SynthesizeFreeClusters(
                output.data(), output.size(), offset);
            (observers.RecordBounce(), ...);
            return STATUS_SUCCESS;
        };

        // This 5 MIB capacity comfortably accommodates a 4 MiB PBS chunk
        // and any extra bytes needed for sector alignment.
        constexpr auto kRetainedReadBufferSize = 5uz * 1024 * 1024;
        if (read_length <= kRetainedReadBufferSize) {
            thread_local const auto retained_buffer_storage = [&failure] {
                auto buffer = NewAlignedBuffer(
                    kRetainedReadBufferSize, read_buffer_alignment);
                if (!buffer) {
                    failure(ERROR_NOT_ENOUGH_MEMORY);
                    std::_Exit(ERROR_NOT_ENOUGH_MEMORY);
                }
                return buffer;
            }();
            return read_into_bounce_buffer_and_copy_to_destination(
                retained_buffer_storage.get());
        }
        const auto newly_allocated_memory = NewAlignedBuffer(
            read_length, read_buffer_alignment);
        if (!newly_allocated_memory) {
            return failure(ERROR_NOT_ENOUGH_MEMORY);
        }
        return read_into_bounce_buffer_and_copy_to_destination(
            newly_allocated_memory.get());
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
            WinError("failed to identify the filesystem for '{}' ({})",
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
        .cluster_shift = CountTrailingZeros(cluster_size),
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

auto OpenBlockDevice(
    const std::filesystem::path &filename,
    const std::string_view description,
    const bool inherit_handle) -> wil::unique_hfile {
    auto attributes = SECURITY_ATTRIBUTES{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = nullptr,
        .bInheritHandle = inherit_handle,
    };
    auto handle = wil::unique_hfile(CreateFileW(filename.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        &attributes, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
        nullptr));
    if (!handle) {
        WinError("failed to open block device '{}' for {}",
            std::wstring_view{filename.native()}, description);
    }
    return handle;
}

// Retrieve the mount manager's opaque device ID through `device`.
export [[nodiscard]] auto QueryMountManagerId(const HANDLE device)
    -> std::expected<std::vector<BYTE>, DWORD> {
    // Windows SDKs 26100 and 28000 omit the mount manager query declarations
    // supplied by the WDK in `mountdev.h`. Declaring them locally allows
    // DeviceFs to issue the query without requiring the WDK to build.
    constexpr auto kQueryUniqueId =
        CTL_CODE('M', 0, METHOD_BUFFERED, FILE_ANY_ACCESS);
    struct {
        USHORT length;
        BYTE bytes[1];
    } header{};
    // The documented minimum output size is `sizeof(MOUNTDEV_UNIQUE_ID)`,
    // so the length probe uses the full structure even though it only
    // needs `UniqueIdLength`.
    // https://learn.microsoft.com/windows-hardware/drivers/ddi/mountdev/ni-mountdev-ioctl_mountdev_query_unique_id#output-buffer-length
    const auto probe_error = Ioctl(
        device, kQueryUniqueId, &header, sizeof(header));
    if ((probe_error != ERROR_SUCCESS) && (probe_error != ERROR_MORE_DATA)) {
        return std::unexpected{probe_error};
    }
    constexpr auto kIdOffset =
        CompileTimeCast<DWORD, offsetof(decltype(header), bytes)>();
    const auto output_size = std::max(
        CompileTimeCast<DWORD, sizeof(header)>(), kIdOffset + header.length);
    const auto output = NEW_MALLOCA_BUFFER(BYTE, output_size);
    if (!output) {
        return std::unexpected{ERROR_NOT_ENOUGH_MEMORY};
    }

    if (const auto error =
            Ioctl(device, kQueryUniqueId, output.get(), output_size);
        error != ERROR_SUCCESS) {
        return std::unexpected{error};
    }
    const auto length =
        std::start_lifetime_as<decltype(header)>(output.get())->length;
    return std::span{
        std::start_lifetime_as_array<BYTE>(
            output.get() + kIdOffset, length),
        length} | std::ranges::to<std::vector<BYTE>>();
}

auto WindowsBlockDevice::FromHandle(
    wil::unique_hfile handle, std::filesystem::path filename,
    const bool extended_dasd, const bool cache,
    const bool synthetic_free_clusters,
    const std::string_view description) -> WindowsBlockDevice {
    const auto alignment = QueryBufferAlignment(handle.get());
    if (!alignment) {
        WinError("failed to query buffer alignment for block device '{}' ({})",
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
        devicefs::WriteToStream(
            devicefs::stderr,
            L"devicefs: warning: FSCTL_ALLOW_EXTENDED_DASD_IO failed for '{}': {}\n",
            std::wstring_view{filename.native()},
            TryConstructWinError("", ExplicitWin32Error{dasd_error}));
    }

    if (cache || synthetic_free_clusters) {
        auto file_system_flags = DWORD{};
        if (!GetVolumeInformationByHandleW(handle.get(), nullptr, 0, nullptr,
                nullptr, &file_system_flags, nullptr, 0)) {
            WinError("failed to query filesystem flags for '{}' ({})",
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
