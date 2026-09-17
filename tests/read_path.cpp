// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

// Verify allocation-bitmap searches, synthetic zeroing, and sector rounding in
// the Windows device read implementation. The bitmap tests compare production
// results with a reference that examines individual bits. The read tests compare
// bytes returned by `WindowsBlockDevice::Read` with the contents of a known file.
//
// Run `devicefs-read-path-test` without arguments. The read tests create a file
// in the system temporary directory that Windows deletes when its handle closes,
// including if a comparison throws. Each completed test group prints a PASS
// message; a failure prints its details and makes the program return 1.

#include <windows.h>
#include <winioctl.h>
#include <wil/safecast.h>

import std;
import <cstddef>;
import devicefs.allocation;
import devicefs.common;
import devicefs.windows_block_device;

namespace {

// Output buffers begin with this nonzero marker so that missed zeroing is
// observable. Its exact value is arbitrary. Each buffer also has one extra byte
// before and after the requested output; those bytes must retain this value.
constexpr auto kSentinelByte = BYTE{0xa5};

// Check that the allocator accepts a one-byte alignment requirement as well as
// larger alignments, and that the returned addresses satisfy those requirements.
auto TestAlignedAllocations() -> void {
    const auto check = []<class T>(const std::size_t alignment) {
        const auto storage = NewAlignedArray<T, false>(1, std::align_val_t{alignment});
        [[gsl::suppress("26490",
            justification:
                "The test converts the pointer to an integer to check whether its "
                "address is divisible by the requested alignment; it does not "
                "access the allocation through a different pointer type.")]]
        if (!storage || ((reinterpret_cast<std::uintptr_t>(storage.get()) % alignment) != 0)) {
            throw std::runtime_error(std::format(
                "the allocator did not provide {}-byte alignment", alignment));
        }
    };
    for (const auto alignment : {1uz, 512uz, 4096uz}) {
        check.operator()<BYTE>(alignment);
        check.operator()<std::byte>(alignment);
        std::println("PASS: BYTE and std::byte allocations satisfy {}-byte alignment.", alignment);
    }
}

// Construct a production allocation-bitmap object from supplied bits and cluster
// geometry. The low bit of the first byte describes cluster zero, and a set bit
// marks an allocated cluster. `bits` must contain enough bytes to describe
// `cluster_count` clusters; `cluster_size` must be a nonzero power of two.
//
// Supplying the bitmap directly lets the tests choose exact allocation patterns
// and partial final words without having to arrange those allocations on NTFS.
template <typename Bitmap = decltype(devicefs::WindowsBlockDevice::allocation_bitmap)>
[[nodiscard]] auto MakeBitmap(const std::span<const BYTE> bits,
    const std::uint64_t cluster_count, const std::uint32_t cluster_size) {
    constexpr auto header_size = offsetof(VOLUME_BITMAP_BUFFER, Buffer);
    auto bitmap = Bitmap{
        .cluster_size = cluster_size,
        .cluster_shift = std::countr_zero(cluster_size),
        .cluster_count = cluster_count,
        .storage = std::make_unique_for_overwrite<BYTE[]>(header_size + bits.size()),
    };
    std::ranges::copy(bits,
        std::span{bitmap.storage.get(), header_size + bits.size()}.subspan(header_size).begin());
    // The filesystem adds this member only when free-cluster measurement is
    // enabled. Making `Bitmap` a template parameter lets this fixture compile
    // with either definition of the production type.
    if constexpr (requires { bitmap.measurement; }) {
        bitmap.measurement = std::make_unique<typename decltype(bitmap.measurement)::element_type>(
            bits, cluster_size, cluster_count);
    }
    return bitmap;
}

// Check `SynthesizeFreeClusters` and `HasAllocatedClusters` for the byte range
// [offset, offset + count). Compute the expected result one byte at a time from
// `bits`, independently of the production search through 64-bit words. Bytes in
// clusters that the bitmap does not represent must retain their contents because
// the bitmap does not establish that they are free. Throw if either result differs.
auto CheckRange(const auto &bitmap, const std::span<const BYTE> bits,
    const std::size_t offset, const std::size_t count) -> void {
    auto actual = std::vector<BYTE>(count + 2, kSentinelByte);
    bitmap.SynthesizeFreeClusters(std::span{actual}.subspan(1, count), offset);
    if ((actual.front() != kSentinelByte) || (actual.back() != kSentinelByte)) {
        throw std::runtime_error("bitmap synthesis wrote outside the output span");
    }

    auto any_allocated = false;
    for (auto index = 0uz; index < count; ++index) {
        const auto cluster = (offset + index) / bitmap.cluster_size;
        const auto allocated = (cluster >= bitmap.cluster_count) ||
            ((bits[cluster / 8] & (1u << (cluster % 8))) != 0);
        any_allocated |= allocated;
        if (actual.at(index + 1) != (allocated ? kSentinelByte : BYTE{})) {
            throw std::runtime_error(std::format(
                "bitmap synthesis differs from the per-bit result: {} clusters, "
                "{} bytes per cluster, offset {}, count {}, byte {}",
                bitmap.cluster_count, bitmap.cluster_size, offset, count, index));
        }
    }
    if ((count != 0) && (bitmap.HasAllocatedClusters(offset, count) != any_allocated)) {
        throw std::runtime_error(std::format(
            "allocated-cluster search differs from the per-bit result: "
            "{} clusters, {} bytes per cluster, offset {}, count {}",
            bitmap.cluster_count, bitmap.cluster_size, offset, count));
    }
}

// Check the supplied bitmap at every starting byte with lengths around cluster
// and bitmap-word boundaries. Four-byte clusters keep this exhaustive traversal
// small while allowing partial-cluster reads. The range extends two clusters
// beyond the bitmap so that comparisons also cover the unrepresented device tail.
auto CheckBitmap(const std::span<const BYTE> bits,
    const std::uint64_t cluster_count) -> void {
    constexpr auto cluster_size = std::uint32_t{4};
    const auto bitmap = MakeBitmap(bits, cluster_count, cluster_size);
    const auto end = (cluster_count + 2) * cluster_size;
    for (auto first = 0uz; first <= end; ++first) {
        for (const auto length : {0uz, 1uz, 3uz, 4uz, 7uz, 8uz, 31uz, 32uz,
                 63uz, 64uz, 255uz, 256uz, 257uz, 511uz, 512uz, 4096uz}) {
            CheckRange(bitmap, bits, first, std::min(length, end - first));
        }
    }
}

// Exercise bitmap searching and zeroing with exhaustive one-byte patterns,
// uniform and alternating regions, isolated differing bits, and random ranges.
auto TestBitmaps() -> void {
    for (auto pattern = 0u; pattern <= 255; ++pattern) {
        const auto bits = std::array{wil::safe_cast_failfast<BYTE>(pattern)};
        CheckBitmap(bits, 8);
    }
    std::println("PASS: all 256 one-byte bitmap patterns match the per-bit search and zeroing.");

    // These maps end on both sides of 64-bit word boundaries. Their final bytes
    // also contain varying bits beyond `cluster_count`; those bits must not
    // affect the result for the device tail that the bitmap does not describe.
    for (const auto clusters : {0uz, 1uz, 7uz, 9uz, 63uz, 64uz, 65uz,
             127uz, 128uz, 129uz, 1024uz}) {
        for (const auto pattern : {BYTE{0}, BYTE{0xff}, BYTE{0x55}, BYTE{0xaa}}) {
            const auto bits = std::vector<BYTE>((clusters + 7) / 8, pattern);
            CheckBitmap(bits, clusters);
        }
    }
    std::println("PASS: uniform and alternating bitmaps match at partial-byte and "
        "64-bit word boundaries, including reads beyond the cluster count.");

    // A 129-cluster map has two complete words and one bit in a third word.
    // Moving the only differing bit through that map checks that word skipping
    // does not overlook a single allocated cluster or a single free cluster.
    for (auto bit = 0uz; bit < 129; ++bit) {
        for (const auto pattern : {BYTE{0}, BYTE{0xff}}) {
            auto bits = std::vector<BYTE>((129 + 7) / 8, pattern);
            bits.at(bit / 8) ^= wil::safe_cast_failfast<BYTE>(1u << (bit % 8));
            CheckBitmap(bits, 129);
        }
    }
    std::println("PASS: each isolated allocated or free cluster in a 129-cluster "
        "bitmap produces the expected search and zeroing results.");

    // The fixed seed makes a failing random case repeatable. Random geometry
    // extends the checks to larger clusters, while a 64 KiB limit on each range
    // keeps the byte-by-byte reference comparison inexpensive.
    auto random = std::mt19937_64{20260917};
    auto octet = std::uniform_int_distribution<unsigned>{0, 255};
    for (auto trial = 0; trial < 1000; ++trial) {
        const auto clusters = random() % 2049;
        const auto cluster_size = std::uint32_t{1} << (random() % 13);
        auto bits = std::vector<BYTE>((clusters + 7) / 8);
        std::ranges::generate(bits, [&random, &octet] {
            return wil::safe_cast_failfast<BYTE>(octet(random));
        });
        const auto bitmap = MakeBitmap(bits, clusters, cluster_size);
        const auto end = (clusters + 2) * cluster_size;
        for (auto range = 0; range < 20; ++range) {
            const auto offset = random() % end;
            CheckRange(bitmap, bits, offset,
                std::min<std::size_t>(random() % 65537, end - offset));
        }
    }
    std::println("PASS: 20,000 random ranges match the per-bit reference with "
        "cluster sizes from 1 to 4096 bytes.");
}

// Check `WindowsBlockDevice::Read` against a file in the system temporary directory.
// Assigning `sector_size` explicitly exercises the production rounding and buffer
// selection for both sector sizes, independently of the test volume's geometry.
// Each read must return the exact requested bytes and leave the adjacent sentinel
// bytes unchanged. The file is deleted when the device handle closes.
auto TestReads() -> void {
    auto contents = std::vector<BYTE>(8uz * 1024 * 1024);
    // Including higher offset bits prevents the contents from repeating every
    // 256 bytes, which could conceal a read from the wrong sector.
    for (auto index = 0uz; index < contents.size(); ++index) {
        contents.at(index) = wil::safe_cast_failfast<BYTE>(
            (index ^ (index >> 8) ^ (index >> 16)) & 0xff);
    }
    const auto filename = std::filesystem::temp_directory_path() /
        std::format("devicefs-read-path-{}.tmp", GetCurrentProcessId());
    auto device = [&filename, length = contents.size()] {
        auto handle = decltype(devicefs::WindowsBlockDevice::handle){CreateFileW(
            filename.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, CREATE_NEW,
            FILE_FLAG_OVERLAPPED | FILE_FLAG_DELETE_ON_CLOSE | FILE_ATTRIBUTE_TEMPORARY,
            nullptr)};
        if (!handle) {
            WinError("could not create read-test file '{}'",
                std::wstring_view{filename.native()});
        }
        const auto alignment = QueryBufferAlignment(handle.get());
        if (!alignment) {
            WinError("could not query read-test buffer alignment");
        }
        return devicefs::WindowsBlockDevice{
            .length = length,
            .filename = filename,
            .handle = std::move(handle),
            .buffer_alignment = *alignment
        };
    }();
    devicefs::WindowsBlockDevice::read_buffer_alignment = device.buffer_alignment;
    auto operation = OVERLAPPED{};
    if (!WriteFile(device.handle.get(), contents.data(),
            wil::safe_cast_failfast<DWORD>(contents.size()), nullptr, &operation)) {
        const auto error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            WinError("could not write read-test file '{}'",
                std::wstring_view{filename.native()}, ExplicitWin32Error{error});
        }
    }
    auto written = DWORD{};
    if (!GetOverlappedResult(device.handle.get(), &operation, &written, TRUE)) {
        WinError("could not finish writing read-test file '{}'",
            std::wstring_view{filename.native()});
    }
    if (written != contents.size()) {
        throw std::runtime_error("the read-test file write was incomplete");
    }

    const auto check = [&device, &contents](const std::uint64_t offset,
                           const std::size_t count) {
        auto output = std::vector<BYTE>(count + 2, kSentinelByte);
        auto transferred = ULONG{};
        const auto status = device.Read(std::span{output}.subspan(1, count).data(),
            offset, wil::safe_cast_failfast<ULONG>(count), transferred);
        if ((status != 0) || (transferred != count) ||
            (output.front() != kSentinelByte) || (output.back() != kSentinelByte) ||
            !std::ranges::equal(std::span{output}.subspan(1, count),
                std::span{contents}.subspan(offset, count))) {
            throw std::runtime_error(std::format(
                "read differs from the source: {}-byte sectors, offset {}, "
                "count {}, status {}, transferred {}",
                device.sector_size, offset, count, status, transferred));
        }
    };
    // Offsets and lengths straddle sector boundaries. Lengths around 5 MiB also
    // exercise both the retained thread-local buffer and a temporary allocation;
    // rounding an unaligned request can move it above that buffer's capacity.
    for (const auto sector_size : {512u, 4096u}) {
        device.sector_size = sector_size;
        for (const auto offset : {0uz, 1uz, 511uz, 512uz, 513uz, 4095uz, 4096uz, 4097uz}) {
            for (const auto count : {1uz, 511uz, 512uz, 513uz, 4095uz, 4096uz,
                     4097uz, (5uz * 1024 * 1024) - 1, 5uz * 1024 * 1024,
                     (5uz * 1024 * 1024) + 1}) {
                check(offset, count);
            }
        }
        std::println("PASS: reads with {}-byte sectors match the source around "
            "sector boundaries and the 5 MiB retained-buffer limit.", sector_size);
        // These requests end at EOF. Rounding must still produce a backing read
        // that the file can satisfy in full.
        check(contents.size() - 1, 1);
        check(contents.size() - sector_size - 1, sector_size + 1);
        std::println("PASS: reads ending at the file's final byte match the source "
            "with {}-byte sectors.", sector_size);
    }
}

} // namespace

auto main() -> int try {
    TestAlignedAllocations();
    TestBitmaps();
    TestReads();
    return 0;
} catch (const std::exception &error) {
    std::println("FAIL: {}", error.what());
    return 1;
}
