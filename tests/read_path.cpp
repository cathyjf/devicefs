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

#include <devicefs/strsafe_compat.h>

import std;
import devicefs.terminal.safecast;
import <devicefs/windows_imports.h>;
import devicefs.allocation;
import devicefs.bitmap_set;
import devicefs.byte_comparison;
import devicefs.count_zeros;
import <devicefs/common.h>;
import devicefs.windows_block_device;
import devicefs.supervisor.math;

#ifdef _M_X64
import <isa_availability.h>;
#endif

namespace {

// Output buffers begin with this nonzero marker so that missed zeroing is
// observable. Its exact value is arbitrary. Each buffer also has one extra byte
// before and after the requested output; those bytes must retain this value.
constexpr auto kSentinelByte = BYTE{0xa5};

// Check that the allocator accepts a one-byte alignment requirement as well as
// larger alignments, and that the returned addresses satisfy those requirements.
auto TestAlignedAllocations() -> void {
    const auto check = []<class T>(const std::size_t alignment) {
        const auto storage = NewAlignedBuffer<T>(1, std::align_val_t{alignment});
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
template <typename Bitmap =
    decltype(devicefs::WindowsBlockDevice<true>::allocation_bitmap)>
[[nodiscard]] auto MakeBitmap(const std::span<const BYTE> bits,
    const std::uint64_t cluster_count, const std::uint32_t cluster_size) {
    constexpr auto header_size = offsetof(VOLUME_BITMAP_BUFFER, Buffer);
    auto bitmap = Bitmap{
        .cluster_size = cluster_size,
        .cluster_shift = devicefs::CountTrailingZeros(cluster_size),
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
    if (count != 0) {
        bitmap.SynthesizeFreeClusters(actual.data() + 1, count, offset);
    }
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
    const auto first = std::min<std::uint64_t>(
        bitmap.cluster_count, offset / bitmap.cluster_size);
    const auto limit = count == 0 ? first : std::min<std::uint64_t>(
        bitmap.cluster_count, ((offset + count - 1) / bitmap.cluster_size) + 1);
    // Both `std::min` calls cap the search at `bitmap.cluster_count`.
    // For an empty byte range, `limit` equals `first`. Otherwise, the
    // range's last byte is at or after its first byte, so its exclusive
    // cluster limit is greater than its starting cluster. Capping both
    // endpoints at the same cluster count preserves their order.
    _Analysis_assume_((first <= limit) && (limit <= bitmap.cluster_count));
    for (const auto allocated : {false, true}) {
        auto expected = first;
        while ((expected != limit) &&
            (((bits[expected >> 3] & (1u << (expected & 7))) != 0) !=
                allocated)) {
            ++expected;
        }
        const auto actual_ = allocated
            ? bitmap.template FindNextCluster<true>(first, limit)
            : bitmap.template FindNextCluster<false>(first, limit);
        if (actual_ != expected) {
            throw std::runtime_error(std::format(
                "cluster search differs from the per-bit result: "
                "{} clusters, range [{}, {}), allocated {}, "
                "expected {}, got {}",
                bitmap.cluster_count, first, limit,
                allocated, expected, actual_));
        }
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
        const auto bits = std::array{
            devicefs::terminal::FailFastCast<BYTE>(pattern)};
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
            bits.at(bit / 8) ^=
                devicefs::terminal::FailFastCast<BYTE>(1u << (bit % 8));
            CheckBitmap(bits, 129);
        }
    }
    std::println("PASS: each isolated allocated or free cluster in a 129-cluster "
        "bitmap produces the expected search and zeroing results.");

    // Two Boolean comparison blocks follow the initial word, and the final
    // cluster occupies a partial word. Moving one differing bit through the map
    // checks both query types; ranges ending at that bit exclude it.
    constexpr auto comparison_clusters = 2113uz;
    for (const auto pattern : {BYTE{}, BYTE{0xff}}) {
        auto bits = std::vector<BYTE>(devicefs::math::Ceil<
            comparison_clusters, std::numeric_limits<BYTE>::digits>(), pattern);
        for (auto bit = 0uz; bit <= comparison_clusters; ++bit) {
            if (bit != comparison_clusters) {
                bits.at(bit >> 3) ^=
                    devicefs::terminal::FailFastCast<BYTE>(1u << (bit & 7));
            }
            const auto bitmap = MakeBitmap(bits, comparison_clusters, 1);
            for (const auto first : {0uz, 1uz, 7uz, 8uz, 63uz, 64uz, 65uz}) {
                for (const auto limit : {bit,
                         std::min(bit + 1, comparison_clusters),
                         comparison_clusters}) {
                    if (first <= limit) {
                        CheckRange(bitmap, bits, first, limit - first);
                    }
                }
            }
            if (bit != comparison_clusters) {
                bits.at(bit >> 3) ^=
                    devicefs::terminal::FailFastCast<BYTE>(1u << (bit & 7));
            }
        }
    }
    std::println("PASS: comparison-block searches find every isolated "
        "allocated or free cluster and exclude matches outside the "
        "requested range.");

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
            return devicefs::terminal::FailFastCast<BYTE>(octet(random));
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

// Compare bitmap membership, cardinality, and stored bits against `std::set`
// after insertion, range insertion, merge, erasure, and clearing. Three-byte blocks
// exercise alignment that cannot be implemented with a power-of-two mask.
auto TestBitmapSet() -> void {
    constexpr auto block_size = std::uint64_t{3};
    constexpr auto maximum_bit = 8192uz;
    constexpr auto maximum_offset = maximum_bit * block_size;
    using Bitmap = devicefs::BitmapSet<block_size>;
    const auto check = [](const auto &actual, const auto &expected) static {
        if ((actual.size() != expected.size()) ||
            (actual.empty() != expected.empty())) {
            throw std::runtime_error("bitmap-set cardinality differs from the reference");
        }
        const auto words = actual.storage();
        constexpr auto bits_per_word = std::numeric_limits<std::uint64_t>::digits;
        const auto limit = expected.empty()
            ? 0uz : ((*expected.rbegin() / block_size) + 1);
        for (auto bit = 0uz; bit < limit; ++bit) {
            const auto offset = bit * block_size;
            const auto word = bit / bits_per_word;
            const auto stored = (word < words.size()) &&
                ((words[word] & (std::uint64_t{1} << (bit % bits_per_word))) != 0);
            if ((actual.contains(offset) != expected.contains(offset)) ||
                (stored != expected.contains(offset))) {
                throw std::runtime_error(std::format(
                    "bitmap-set membership differs at offset {}", offset));
            }
        }
    };
    auto aligned_maximum = Bitmap(maximum_offset);
    if (!aligned_maximum.insert(maximum_offset)) {
        throw std::runtime_error(
            "bitmap-set insertion rejected its aligned maximum offset");
    }
    check(aligned_maximum, std::set{maximum_offset});

    auto actual = Bitmap(maximum_offset + block_size - 1);
    auto expected = std::set<std::uint64_t>{};
    check(actual, expected);
    const auto insert = [&actual, &expected, &check](const auto bit) {
        const auto offset = bit * block_size;
        if (actual.insert(offset) != expected.insert(offset).second) {
            throw std::runtime_error("bitmap-set insertion reported the wrong membership change");
        }
        check(actual, expected);
    };
    for (const auto bit : {0uz, 63uz, 64uz, 65uz, 129uz, 64uz}) {
        insert(bit);
    }
    for (const auto [first, limit] :
        {std::pair{62uz, 67uz}, std::pair{64uz, 128uz},
            std::pair{129uz, 129uz}, std::pair{190uz, 385uz}}) {
        actual.insert_range(first * block_size, limit * block_size);
        for (auto bit = first; bit < limit; ++bit) {
            expected.insert(bit * block_size);
        }
        check(actual, expected);
    }
    auto source = Bitmap{block_size, 64 * block_size, 130 * block_size};
    auto reference_source = decltype(expected){
        block_size, 64 * block_size, 130 * block_size};
    actual.merge(source);
    expected.merge(reference_source);
    check(actual, expected);
    check(source, reference_source);
    for (const auto bit : {0uz, 64uz, 64uz, 130uz}) {
        if (actual.erase(bit * block_size) != expected.erase(bit * block_size)) {
            throw std::runtime_error("bitmap-set erasure reported the wrong membership change");
        }
        check(actual, expected);
    }
    for (const auto bit : {1024uz, 4096uz, maximum_bit}) {
        insert(bit);
    }
    actual.clear();
    expected.clear();
    check(actual, expected);
    insert(maximum_bit);
    std::println("PASS: compact candidate maps preserve membership, counts, "
        "word boundaries, overlapping ranges, merge intersections, "
        "growth, and clear-and-reinsert behavior.");
}

template <unsigned Feature>
struct IsaSupportFor {
    constexpr static auto operator()(const unsigned feature, const unsigned)
        noexcept -> bool {
        return feature == Feature;
    }
};

// Check byte counts against a scalar reference, including independently
// unaligned inputs, partial vectors, and totals larger than a byte can hold.
auto TestDifferingByteCounts(const auto &count_differing_bytes,
    const std::string_view implementation) -> void {
    constexpr auto maximum_size = 4097uz;
    constexpr auto alignment_count = 64uz;
    constexpr auto boundary_size = 4 * alignment_count + 1;
    auto left = std::vector<unsigned char>(maximum_size + alignment_count);
    auto right = left;
    auto random = std::mt19937{0x9c37};
    for (auto index = 0uz; index < left.size(); ++index) {
        left[index] = devicefs::terminal::FailFastCast<unsigned char>(
            random() & 0xff);
        right[index] = devicefs::terminal::FailFastCast<unsigned char>(
            random() & 0xff);
    }
    const auto check = [&left, &right, &count_differing_bytes, implementation](
        const auto left_offset, const auto right_offset, const auto size) {
        __pragma(warning(push))
        __pragma(warning(disable : 26429, justification :
            "Nullable pointers `first` and `second` are required to perform "
            "pointer arithmetic."))
        const auto *const first = left.data() + left_offset;
        const auto *const second = right.data() + right_offset;
        __pragma(warning(pop))
        auto expected = 0uz;
        for (auto index = 0uz; index < size; ++index) {
            expected += first[index] != second[index];
        }
        if (count_differing_bytes(first, second, size) != expected) {
            throw std::runtime_error(std::format(
                "{} differing-byte count is wrong for offsets {} and {}, "
                "length {}", implementation, left_offset, right_offset, size));
        }
    };
    for (auto left_offset = 0uz; left_offset < alignment_count;
        ++left_offset) {
        for (auto right_offset = 0uz; right_offset < alignment_count;
            ++right_offset) {
            for (auto size = 0uz; size <= boundary_size; ++size) {
                check(left_offset, right_offset, size);
            }
            check(left_offset, right_offset, maximum_size);
        }
    }
    right = left;
    check(0uz, 0uz, maximum_size);
    for (auto index = 0uz; index < boundary_size; ++index) {
        right[index] ^= 0xff;
        check(0uz, 0uz, boundary_size);
        right[index] = left[index];
    }
    for (auto index = 0uz; index < right.size(); ++index) {
        right[index] ^= 0xff;
    }
    check(0uz, 0uz, maximum_size);
    left.assign(4uz * 1024 * 1024, 0);
    right.assign(left.size(), 0xff);
    check(0uz, 0uz, left.size());
    std::println(
        "PASS: {} differing-byte counts match the scalar reference.",
        implementation);
}

// Compare each meaningful output bit with a scalar projection of the input.
// Subspans and partial final groups determine which source bits participate;
// padding bits and additional reserved bytes have no required values.
template <std::size_t new_resolution>
auto TestBitmapReductions(const auto &reduce_bitmap,
    const auto &dispatch_reduce_bitmap,
    const std::string_view implementation) -> void {
    constexpr auto word_bits = std::numeric_limits<std::uint64_t>::digits;
    const auto check = [implementation](const auto &words,
        const std::size_t bits_per_region, const auto &result,
        const std::size_t minimum_length) {
        constexpr auto element_bits = sizeof(words[0]) *
            std::numeric_limits<unsigned char>::digits;
        const auto input_bits = words.size() * element_bits;
        const auto output_bits = input_bits / bits_per_region +
            ((input_bits % bits_per_region) != 0);
        auto expected = std::vector<unsigned char>(
            (output_bits / 8) + ((output_bits % 8) != 0));
        for (auto bit = 0uz; bit < input_bits; ++bit) {
            if (((words[bit / element_bits] >> (bit % element_bits)) & 1) != 0) {
                const auto output_bit = bit / bits_per_region;
                expected[output_bit / 8] |= 1u << (output_bit % 8);
            }
        }
        auto matches = result.size() == std::max(expected.size(), minimum_length);
        for (auto bit = 0uz; matches && (bit < output_bits); ++bit) {
            matches = ((result[bit / 8] ^ expected[bit / 8]) &
                (1u << (bit % 8))) == 0;
        }
        if (!matches) {
            throw std::runtime_error(std::format(
                "{} bitmap reduction is wrong for {} source elements, "
                "resolution ratio {}, minimum length {}",
                implementation, words.size(), bits_per_region, minimum_length));
        }
    };
    auto random = std::mt19937{0x5e17};
    const auto test_resolution = [&reduce_bitmap, &check, &random]
        <std::size_t bits_per_region> {
        const auto check_result = [&reduce_bitmap, &check](
            const std::span<const std::uint64_t> words,
            const std::size_t minimum_length = 0) {
            const auto result = reduce_bitmap.template operator()<bits_per_region>(
                words, minimum_length);
            check(words, bits_per_region, result, minimum_length);
        };
        constexpr auto words_per_region = bits_per_region / word_bits;
        for (const auto regions : {1uz, 3uz, 4uz, 5uz, 7uz, 8uz, 9uz,
                 15uz, 16uz, 17uz, 31uz, 32uz, 33uz, 63uz, 64uz, 65uz}) {
            for (const auto remainder :
                 std::views::iota(1uz, words_per_region + 1)) {
                const auto word_count = (regions - 1) * words_per_region + remainder;
                const auto input_bits = word_count * word_bits;
                auto storage = std::vector<std::uint64_t>(word_count + 1);
                // Offset the source by one word to exercise unaligned SIMD loads.
                auto words = std::span{storage}.subspan(1);
                check_result(words);
                for (const auto region : std::views::iota(0uz, regions)) {
                    if ((random() & 1) != 0) {
                        const auto first = region * bits_per_region;
                        const auto bit = first + random() %
                            std::min(bits_per_region, input_bits - first);
                        words[bit / word_bits] |=
                            std::uint64_t{1} << (bit % word_bits);
                    }
                }
                for (const auto prefix : {1uz, 3uz, 4uz, 5uz,
                         (word_count + 1) / 2, word_count}) {
                    check_result(words.first(std::min(prefix, word_count)));
                }
                std::ranges::fill(words, ~std::uint64_t{});
                check_result(words, 1);
            }
        }
        // Check every source bit in partial and complete output bytes.
        for (const auto regions : {4uz, 8uz}) {
            auto words = std::vector<std::uint64_t>(regions * words_per_region);
            for (auto bit = 0uz; bit < words.size() * word_bits; ++bit) {
                words[bit / word_bits] = std::uint64_t{1} << (bit % word_bits);
                check_result(words);
                words[bit / word_bits] = 0;
            }
            words.front() = 1;
            words.back() = std::uint64_t{1} << (word_bits - 1);
            for (const auto minimum_length : {0uz, 1uz, 4096uz}) {
                check_result(words, minimum_length);
            }
        }
    };
    test_resolution.template operator()<128>();
    test_resolution.template operator()<256>();
    test_resolution.template operator()<384>();
    test_resolution.template operator()<512>();
    test_resolution.template operator()<768>();

    for (const auto original_resolution :
         {512uz, 1024uz, 2048uz, 4096uz, 8192uz, 16384uz, 32768uz}) {
        const auto bits_per_region =
            new_resolution / original_resolution;
        const auto bytes_per_region = bits_per_region / 8;
        auto storage = std::vector<unsigned char>(9 * bytes_per_region + 2);
        const auto input = std::span{storage}.subspan(1);
        for (const auto region : std::views::iota(0uz, 10uz)) {
            const auto first = region * bytes_per_region;
            const auto last = std::min(first + bytes_per_region, input.size()) - 1;
            input[first] = 1;
            input[last] |= 0x80;
            for (const auto minimum_length : {0uz, 1uz, 4096uz}) {
                const auto result = dispatch_reduce_bitmap(
                    std::span<const unsigned char>{input}, original_resolution,
                    minimum_length);
                if (!result) {
                    throw std::runtime_error(std::format(
                        "{} bitmap dispatch rejected source resolution {}",
                        implementation, original_resolution));
                }
                check(input, bits_per_region, *result, minimum_length);
            }
            input[first] = input[last] = 0;
        }
    }
    const auto input = std::to_array<unsigned char>({1});
    if (dispatch_reduce_bitmap(std::span{input}, 65536, 0)) {
        throw std::runtime_error(std::format(
            "{} bitmap dispatch accepted unsupported source resolution 65536",
            implementation));
    }
    std::println("PASS: {} bitmap reductions match the scalar reference.",
        implementation);
}

// Check `WindowsBlockDevice::Read` against a file in the system temporary directory.
// Assigning `sector_size` explicitly exercises the production rounding and buffer
// selection for both sector sizes, independently of the test volume's geometry.
// Each read must return the exact requested bytes and leave the adjacent sentinel
// bytes unchanged. The file is deleted when the device handle closes.
template <bool HasAllocationBitmap>
auto TestReads() -> void {
    using Device = devicefs::WindowsBlockDevice<HasAllocationBitmap>;
    auto contents = std::vector<BYTE>(8uz * 1024 * 1024);
    // Including higher offset bits prevents the contents from repeating every
    // 256 bytes, which could conceal a read from the wrong sector.
    for (auto index = 0uz; index < contents.size(); ++index) {
        contents.at(index) = devicefs::terminal::FailFastCast<BYTE>(
            (index ^ (index >> 8) ^ (index >> 16)) & 0xff);
    }
    const auto filename = std::filesystem::temp_directory_path() /
        std::format("devicefs-read-path-{}.tmp", GetCurrentProcessId());
    auto device = [&filename, length = contents.size()] {
        auto handle = decltype(Device::handle){CreateFileW(
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
        return Device{
            .length = length,
            .filename = filename,
            .handle = std::move(handle),
            .buffer_alignment = *alignment
        };
    }();
    Device::read_buffer_alignment = device.buffer_alignment;
    auto operation = OVERLAPPED{};
    if (!WriteFile(device.handle.get(), contents.data(),
            devicefs::terminal::FailFastCast<DWORD>(contents.size()),
            nullptr, &operation)) {
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

    auto allocation_bits = std::vector<BYTE>{};
    constexpr auto cluster_size = 4096u;
    if constexpr (HasAllocationBitmap) {
        allocation_bits.assign((contents.size() / cluster_size) / 8, BYTE{0xff});
        device.allocation_bitmap = MakeBitmap(allocation_bits,
            contents.size() / cluster_size, cluster_size);
    }
    const auto check = [&device, &contents, &allocation_bits](const std::uint64_t offset,
                           const std::size_t count) {
        const auto wanted = devicefs::terminal::FailFastCast<ULONG>(count);
        if ((wanted == 0) || (offset >= device.length) ||
            (wanted > (device.length - offset))) {
            throw std::invalid_argument(std::format(
                "read-test request exceeds the fixture: offset {}, count {}, length {}",
                offset, wanted, device.length));
        }
        auto expected = std::span{contents}.subspan(offset, count) |
            std::ranges::to<std::vector>();
        if (!allocation_bits.empty()) {
            for (auto index = 0uz; index < count; ++index) {
                const auto cluster = (offset + index) / cluster_size;
                if ((allocation_bits.at(cluster / 8) & (1u << (cluster % 8))) == 0) {
                    expected.at(index) = 0;
                }
            }
        }
        auto output = std::vector<BYTE>(count + 2, kSentinelByte);
        auto transferred = ULONG{};
        const auto status = device.Read(std::span{output}.subspan(1, count).data(),
            offset, wanted, transferred);
        if ((status != 0) || (transferred != count) ||
            (output.front() != kSentinelByte) || (output.back() != kSentinelByte) ||
            !std::ranges::equal(std::span{output}.subspan(1, count),
                expected)) {
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
        check(contents.size() - sector_size - 1,
            CompileTimeCast<std::size_t>(sector_size) + 1);
        std::println("PASS: reads ending at the file's final byte match the source "
            "with {}-byte sectors.", sector_size);
    }
    if constexpr (HasAllocationBitmap) {
        for (const auto pattern : {BYTE{0}, BYTE{0xff}, BYTE{0xaa}}) {
            allocation_bits.assign((contents.size() / cluster_size) / 8,
                pattern);
            device.allocation_bitmap = MakeBitmap(allocation_bits,
                contents.size() / cluster_size, cluster_size);
            for (const auto sector_size : {512u, 4096u}) {
                device.sector_size = sector_size;
                for (const auto offset : {0uz, 1uz, 4095uz}) {
                    for (const auto count : {1uz, 4097uz, 8192uz,
                             (5uz * 1024 * 1024) + 1}) {
                        check(offset, count);
                    }
                }
            }
        }
        std::println("PASS: bitmap-backed reads zero free clusters, retain "
            "allocated contents, report exact counts, and leave surrounding "
            "sentinels unchanged.");
    }
}

} // namespace

auto main() -> int try {
    TestAlignedAllocations();
    TestBitmaps();
    TestReads<false>();
    TestReads<true>();
    TestBitmapSet();
    TestDifferingByteCounts(devicefs::CountDifferingBytes<>, "default");
    constexpr auto kBitmapReductionResolution = 4uz * 1024 * 1024;
    TestBitmapReductions<kBitmapReductionResolution>(
        []<std::size_t Ratio>(auto &&...arguments) static {
            return devicefs::ReduceBitmap<const std::uint64_t,
                16uz * 1024, Ratio * 16uz * 1024>(
                std::forward<decltype(arguments)>(arguments)...);
        }, devicefs::DispatchReduceBitmap<const unsigned char>, "default");
#ifdef _M_X64
    const auto test_supported = []<unsigned Feature>(
        const std::string_view implementation) static {
        if (__check_isa_support(Feature, 0)) {
            TestDifferingByteCounts(
                devicefs::CountDifferingBytes<IsaSupportFor<Feature>>,
                implementation);
            if constexpr (Feature != __IA_SUPPORT_VECTOR512) {
                TestBitmapReductions<kBitmapReductionResolution>(
                    []<std::size_t Ratio>(auto &&...arguments) static {
                        return devicefs::ReduceBitmap<const std::uint64_t,
                            16uz * 1024, Ratio * 16uz * 1024,
                            IsaSupportFor<Feature>>(
                                std::forward<decltype(arguments)>(arguments)...);
                    }, devicefs::DispatchReduceBitmap<const unsigned char,
                        kBitmapReductionResolution, IsaSupportFor<Feature>>,
                    implementation);
            }
        } else {
            std::println("SKIP: {} is not supported on this machine.",
                implementation);
        }
    };
    test_supported.operator()<__IA_SUPPORT_VECTOR128>("SSE2");
    test_supported.operator()<__IA_SUPPORT_VECTOR256>("AVX2");
    test_supported.operator()<__IA_SUPPORT_VECTOR512>("AVX-512BW");
#endif
    return 0;
} catch (const std::exception &error) {
    std::println("FAIL: {}", error.what());
    return 1;
}
