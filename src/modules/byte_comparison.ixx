// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.byte_comparison;

import std;
import devicefs.bitmap_set;
import <devicefs/common.h>;
import <devicefs/windows_imports.h>;

#ifdef _M_X64
import <isa_availability.h>;
#endif // #ifdef _M_X64

#ifdef _M_ARM64
__pragma(warning(push))
__pragma(warning(disable : 5244, justification:
    "Including this header with C++ linkage attaches its symbols to the "
    "global module, not the named module. Including this header after the "
    "imports allows it to benefit from the symbols already defined in the "
    "main Windows header unit."))
extern "C++" {
#include <arm64_neon.h>
}
__pragma(warning(pop))
#endif // #ifdef _M_ARM64

namespace devicefs::byte_comparison_detail {

template <class Vector>
[[msvc::forceinline]]
auto LoadVector(const ByteType auto *const input) noexcept {
#ifdef _M_ARM64
    return vld1q_u8(input);
#elifdef _M_X64
    if constexpr (std::same_as<Vector, __m512i>) {
        return _mm512_loadu_si512(input);
    } else {
        using Bytes = char[sizeof(Vector)];
        // The `std::bit_cast` function does not require a live object
        // of the source type. It requires only that each bit within the
        // representation of the source be within some live object.
        // Here, that requirement is met because each bit is within a live
        // `unsigned char` object in a byte buffer or a live `std::uint64_t`
        // object in bitmap storage.
        // https://timsong-cpp.github.io/cppwp/n4950/bit.cast
        //
        // Aside from the definition of `std::bit_cast` (which does not
        // disallow this cast), no other identifiable part of the C++
        // standard has been located that requires a live source object
        // of type `Bytes` to exist to perform this cast.
        const auto vector = std::bit_cast<Vector>(
            ByteViewCast<const Bytes &>(*input));
        if constexpr (std::same_as<Vector, __m256i>) {
            return _mm256_loadu_si256(std::addressof(vector));
        } else {
            return _mm_loadu_si128(std::addressof(vector));
        }
    }
#else
#error `LoadVector` lacks an implementation for this architecture
#endif
}

template <class Vector>
[[msvc::forceinline]]
auto VectorBitwiseOr(const Vector left, const Vector right) noexcept {
#ifdef _M_ARM64
    return vorrq_u8(left, right);
#elifdef _M_X64
    if constexpr (std::same_as<Vector, __m512i>) {
        return _mm512_or_si512(left, right);
    } else if constexpr (std::same_as<Vector, __m256i>) {
        return _mm256_or_si256(left, right);
    } else {
        return _mm_or_si128(left, right);
    }
#else
#error `VectorBitwiseOr` lacks an implementation for this architecture
#endif
}

struct VectorBitwiseOrInvoker {
    [[msvc::forceinline]]
    static auto operator()(const auto left, const auto right) noexcept {
        return VectorBitwiseOr(left, right);
    }
};

template <std::size_t group_bytes, class Vector>
constexpr auto kExpandBitmapReduction = (group_bytes <= 128) &&
    std::has_single_bit(group_bytes / sizeof(Vector));

template <class Comparator, class... T>
    requires ((sizeof...(T) >= 2) &&
        std::has_single_bit(sizeof...(T)))
[[msvc::forceinline]]
auto ReducePairwise(const T &...values) noexcept {
    if constexpr (sizeof...(T) == 2) {
        return Comparator::operator()(values...);
    } else {
        constexpr auto half = sizeof...(T) / 2;
        const auto inputs = std::tie(values...);
        return [&inputs]<std::size_t... Index>
            [[msvc::forceinline]] (std::index_sequence<Index...>) {
            return Comparator::operator()(
                ReducePairwise<Comparator>(
                    std::get<Index>(inputs)...),
                ReducePairwise<Comparator>(
                    std::get<half + Index>(inputs)...));
        }(std::make_index_sequence<half>{});
    }
}

#ifdef _M_ARM64
struct ArmVectorMaxInvoker {
    [[msvc::forceinline]]
    static auto operator()(const auto left, const auto right) noexcept {
        return vpmaxq_u8(left, right);
    }
};
#endif // #ifdef _M_ARM64

#ifdef _M_X64
// Compute the bitwise OR of each pair of adjacent 64-bit lanes in each
// input. The results alternate between `left` and `right`.
[[msvc::forceinline]]
auto CombineAdjacentLanes(const auto left, const auto right) noexcept {
    return _mm512_or_si512(_mm512_unpacklo_epi64(left, right),
        _mm512_unpackhi_epi64(left, right));
}

// Compute the bitwise OR of each pair of adjacent 128-bit lanes in each
// input. The results from `left` precede those from `right`.
[[msvc::forceinline]]
auto InterleaveAndBitwiseOr(const auto left, const auto right) noexcept {
    return _mm512_or_si512(
        _mm512_shuffle_i64x2(left, right, _MM_SHUFFLE(2, 0, 2, 0)),
        _mm512_shuffle_i64x2(left, right, _MM_SHUFFLE(3, 1, 3, 1)));
}

// Return eight bitmap bits from four vectors containing two consecutive
// 256-bit regions apiece. Each bit is set when its region is nonzero, with
// bit zero describing the first region of `ab`.
template <class Vector>
    requires (sizeof(Vector) == 64)
[[msvc::forceinline]]
auto ReduceVectorsToBitmapByte(const Vector ab, const Vector cd,
    const Vector ef, const Vector gh) noexcept {
    const auto groups = InterleaveAndBitwiseOr(
        CombineAdjacentLanes(ab, cd), CombineAdjacentLanes(ef, gh));
    // The reduction orders the regions as a, c, b, d, e, g, f, h.
    // Swapping the middle two lanes of each 256-bit half restores source order.
    const auto ordered = _mm512_permutex_epi64(
        groups, _MM_SHUFFLE(3, 1, 2, 0));
    return _mm512_cmpneq_epi64_mask(ordered, _mm512_setzero_si512());
}

template <class Vector>
    requires (sizeof(Vector) == 64)
[[msvc::forceinline]]
auto ReduceVectorsToBitmapByte(const Vector a, const Vector b,
    const Vector c, const Vector d, const Vector e, const Vector f,
    const Vector g, const Vector h) noexcept {
    // Each stage combines pairs of lanes while interleaving the source
    // vectors. The final eight 64-bit lanes each contain the OR of one
    // source vector, in source order.
    const auto groups = InterleaveAndBitwiseOr(
        InterleaveAndBitwiseOr(CombineAdjacentLanes(a, b),
            CombineAdjacentLanes(c, d)),
        InterleaveAndBitwiseOr(CombineAdjacentLanes(e, f),
            CombineAdjacentLanes(g, h)));
    return _mm512_cmpneq_epi64_mask(groups, _mm512_setzero_si512());
}
#endif // #ifdef _M_X64

// Return eight bitmap bits in source order, with one set bit for each nonzero
// vector. Bit zero describes `a`.
template <class Vector>
    requires (sizeof(Vector) < 64)
[[msvc::forceinline]]
auto ReduceVectorsToBitmapByte(const Vector a, const Vector b,
    const Vector c, const Vector d, const Vector e, const Vector f,
    const Vector g, const Vector h) noexcept -> unsigned char {
#ifdef _M_ARM64
    // Pairwise maxima reduce each source vector to one byte. Grouping the
    // pairs in source order preserves the output bit positions.
    const auto pairs = ReducePairwise<ArmVectorMaxInvoker>(
        a, b, c, d, e, f, g, h);
    const auto maxima = vget_low_u8(vpmaxq_u8(pairs, pairs));
    const auto masks = vcgt_u8(maxima, vdup_n_u8(0));
    static constexpr auto weights = std::to_array<unsigned char>({
        1, 2, 4, 8, 16, 32, 64, 128});
    const auto packed = vaddv_u8(vand_u8(masks, vld1_u8(weights.data())));
#elifdef _M_X64
    const auto nibble = [] [[msvc::forceinline]] (
        const Vector first, const Vector second,
        const Vector third, const Vector fourth) static noexcept {
        const auto groups = [first, second, third, fourth] [[msvc::forceinline]] {
            if constexpr (std::same_as<Vector, __m256i>) {
                const auto ab = _mm256_or_si256(
                    _mm256_unpacklo_epi32(first, second),
                    _mm256_unpackhi_epi32(first, second));
                const auto cd = _mm256_or_si256(
                    _mm256_unpacklo_epi32(third, fourth),
                    _mm256_unpackhi_epi32(third, fourth));
                const auto halves = _mm256_or_si256(
                    _mm256_unpacklo_epi64(ab, cd),
                    _mm256_unpackhi_epi64(ab, cd));
                return _mm_or_si128(_mm256_castsi256_si128(halves),
                    _mm256_extracti128_si256(halves, 1));
            } else {
                const auto ab = _mm_or_si128(_mm_unpacklo_epi32(first, second),
                    _mm_unpackhi_epi32(first, second));
                const auto cd = _mm_or_si128(_mm_unpacklo_epi32(third, fourth),
                    _mm_unpackhi_epi32(third, fourth));
                return _mm_or_si128(_mm_unpacklo_epi64(ab, cd),
                    _mm_unpackhi_epi64(ab, cd));
            }
        }();
        const auto masks = _mm_cmpeq_epi32(groups, _mm_setzero_si128());
        return _mm_movemask_ps(_mm_castsi128_ps(masks)) ^ 0xf;
    };
    const auto low = nibble(a, b, c, d);
    const auto high = nibble(e, f, g, h);
    [[gsl::suppress("26472", justification:
        "Each x64 nibble is in [0, 15]. Combining the low nibble with the "
        "high nibble shifted left by four produces a value in [0, 255], "
        "which fits in an `unsigned char` output byte.")]]
    const auto packed = static_cast<unsigned char>(low | (high << 4));
#else
#error `ReduceVectorsToBitmapByte` lacks an implementation for this architecture
#endif
    return packed;
}

#ifdef _M_ARM64
// MSVC 19.51.36260 crashes while writing SVE types to an IFC, including in
// private module fragments. The SVE definitions are compiled in
// `byte_comparison_sve.cpp` instead.

// Reduce each source region to one output bit. Predicated loads keep reads
// within the supplied input, including an incomplete final region.
//
// This function returns false without modifying output if `svcntb()` returns
// less than 16 or greater than 256 or a value that is not a multiple of 16.
// Otherwise, this function returns true.
//
// Precondition: `output` is zero-initialized.
template <std::size_t source_bytes_per_output_bit>
[[nodiscard, msvc::noinline]]
_Pre_satisfies_(size > 0)
auto ReduceBitmapUsingSve(
    _In_reads_bytes_(size) const unsigned char *const input,
    const std::size_t size,
    _Inout_updates_bytes_(size / (source_bytes_per_output_bit *
        std::numeric_limits<unsigned char>::digits) +
        ((size % (source_bytes_per_output_bit *
            std::numeric_limits<unsigned char>::digits)) != 0))
    unsigned char *const output) noexcept -> bool;

#define DEVICEFS_INSTANTIATE_SVE_REDUCTION(Bytes) \
    extern template auto ReduceBitmapUsingSve<Bytes>( \
        const unsigned char *, std::size_t, unsigned char *) noexcept -> bool

#include "byte_comparison_sve.h"

#undef DEVICEFS_INSTANTIATE_SVE_REDUCTION
#endif // #ifdef _M_ARM64

struct IsaSupportInvoker {
    [[msvc::forceinline]]
    static auto operator()(auto &&...arguments) noexcept {
#ifdef _M_ARM64
        return IsProcessorFeaturePresent(
            std::forward<decltype(arguments)>(arguments)...);
#else
        return __check_isa_support(
            std::forward<decltype(arguments)>(arguments)...);
#endif
    }
};

} // namespace devicefs::byte_comparison_detail

namespace devicefs {

#ifdef _M_ARM64
export [[nodiscard]] auto SveVectorBytes() noexcept -> std::size_t;
#endif // #ifdef _M_ARM64

// Count the byte positions at which the two buffers differ.
export
template <class IsaSupport = byte_comparison_detail::IsaSupportInvoker>
[[nodiscard]]
[[gsl::suppress("26429", justification:
    "Pointers `left` and `right` may be null when `size` is zero. "
    "An empty comparison reads neither buffer.")]]
auto CountDifferingBytes(
    _When_(size != 0, _In_reads_bytes_(size))
    const unsigned char *const left,
    _When_(size != 0, _In_reads_bytes_(size))
    const unsigned char *const right,
    const std::size_t size) noexcept -> std::size_t {
#ifdef _M_ARM64
    auto count = 0uz;
    auto offset = 0uz;
    constexpr auto width = sizeof(uint8x16_t);
    for (; (size - offset) >= width; offset += width) {
        const auto equal = vceqq_u8(
            byte_comparison_detail::LoadVector<uint8x16_t>(left + offset),
            byte_comparison_detail::LoadVector<uint8x16_t>(right + offset));
        // The inverted comparison contains `0xff` for each unequal byte.
        // Shifting those values to 1 keeps their sum within a byte.
        count += vaddvq_u8(vshrq_n_u8(vmvnq_u8(equal), 7));
    }
#elifdef _M_X64
    const auto count_vectors = [left, right, size]<class Vector>
        [[msvc::noinline]] {
        constexpr auto width = sizeof(Vector);
        const auto zero = [] [[msvc::forceinline]] static noexcept {
            if constexpr (std::same_as<Vector, __m512i>) {
                return _mm512_setzero_si512();
            } else if constexpr (std::same_as<Vector, __m256i>) {
                return _mm256_setzero_si256();
            } else {
                return _mm_setzero_si128();
            }
        }();
        auto offset = 0uz;
        auto totals = zero;
        for (; (size - offset) >= width; offset += width) {
            const auto left_bytes =
                byte_comparison_detail::LoadVector<Vector>(left + offset);
            const auto right_bytes =
                byte_comparison_detail::LoadVector<Vector>(right + offset);
            // Each unequal byte contributes 1. The sum-of-absolute-differences
            // instructions sum groups of eight bytes into 64-bit lanes, so
            // each lane's accumulated count is at most `size`.
            if constexpr (std::same_as<Vector, __m512i>) {
                const auto different = _mm512_maskz_set1_epi8(
                    _mm512_cmpneq_epi8_mask(left_bytes, right_bytes), 1);
                totals = _mm512_add_epi64(
                    totals, _mm512_sad_epu8(different, zero));
            } else if constexpr (std::same_as<Vector, __m256i>) {
                const auto different = _mm256_andnot_si256(
                    _mm256_cmpeq_epi8(left_bytes, right_bytes),
                    _mm256_set1_epi8(1));
                totals = _mm256_add_epi64(
                    totals, _mm256_sad_epu8(different, zero));
            } else {
                const auto different = _mm_andnot_si128(
                    _mm_cmpeq_epi8(left_bytes, right_bytes),
                    _mm_set1_epi8(1));
                totals = _mm_add_epi64(
                    totals, _mm_sad_epu8(different, zero));
            }
        }
        const auto reduced = [&totals] {
            if constexpr (std::same_as<Vector, __m512i>) {
                return _mm256_add_epi64(
                    _mm512_castsi512_si256(totals),
                    _mm512_extracti64x4_epi64(totals, 1));
            } else {
                return totals;
            }
        }();
        const auto pair = [&reduced] {
            if constexpr (sizeof(reduced) > sizeof(__m128i)) {
                return _mm_add_epi64(
                    _mm256_castsi256_si128(reduced),
                    _mm256_extracti128_si256(reduced, 1));
            } else {
                return reduced;
            }
        }();
        const auto combined = _mm_add_epi64(
            pair, _mm_srli_si128(pair, sizeof(std::uint64_t)));
        // `_mm_cvtsi128_si64` returns a signed integer. This expression
        // interprets its bits as the unsigned count so that values above
        // `INT64_MAX` remain valid.
        const auto count =
            std::bit_cast<std::size_t>(_mm_cvtsi128_si64(combined));
        return std::pair{count, offset};
    };
    auto [count, offset] = [&count_vectors] [[msvc::forceinline]] {
        if (IsaSupport::operator()(__IA_SUPPORT_VECTOR512, 0)) {
            return count_vectors.template operator()<__m512i>();
        }
        if (IsaSupport::operator()(__IA_SUPPORT_VECTOR256, 0)) {
            return count_vectors.template operator()<__m256i>();
        }
        return count_vectors.template operator()<__m128i>();
    }();
#else
#error `CountDifferingBytes` lacks an implementation for this architecture
#endif
    for (; offset < size; ++offset) {
        count += left[offset] != right[offset];
    }
    return count;
}

// The default specialization is compiled in this translation unit so that
// importers do not each instantiate the SIMD implementations.
template auto CountDifferingBytes<>(
    const unsigned char *, const unsigned char *, std::size_t)
    noexcept -> std::size_t;

constexpr auto kDefaultBitmapResolution = 4uz * 1024 * 1024;
using ReducedBitmap = ReallocVector<unsigned char>;

// Reduce a bitmap whose bits each describe `original_resolution` volume bytes
// to one whose bits each describe `new_resolution` bytes. Both maps use the
// low bit of each byte first. Every bit in `storage` participates in the
// reduction. A partial final group produces one output bit.
// The result contains at least `bitmap_minimum_length` bytes. The allocation
// is zero-initialized, including any bytes added to meet that minimum.
// Precondition: `storage` is nonempty.
export template <std::integral T, std::size_t original_resolution,
    std::size_t new_resolution = kDefaultBitmapResolution,
    class IsaSupport = byte_comparison_detail::IsaSupportInvoker>
    requires ((original_resolution != 0) && (new_resolution != 0) &&
        ((new_resolution % original_resolution) == 0) &&
        (((new_resolution / original_resolution) % 128) == 0))
[[nodiscard]]
auto ReduceBitmap(const std::span<T> storage,
    const std::size_t bitmap_minimum_length = 0)
    -> ReducedBitmap {
    constexpr auto bits_per_byte = std::numeric_limits<unsigned char>::digits;
    static_assert(bits_per_byte == 8);
    static_assert(std::endian::native == std::endian::little);
    constexpr auto resolution_ratio = new_resolution / original_resolution;
    static constexpr auto words_per_bit =
        resolution_ratio / (sizeof(T) * bits_per_byte);
    const auto bits = storage.size() / words_per_bit +
        ((storage.size() % words_per_bit) != 0);
    const auto output_size =
        bits / bits_per_byte + ((bits % bits_per_byte) != 0);
    auto result = ReducedBitmap{};
    result.resize(std::max(output_size, bitmap_minimum_length));
    auto *const output = std::addressof(result[0]);
    const auto *const input =
        ByteViewCast<const unsigned char *>(storage.data());

    // With two regions per group, one 512-bit load supplies two adjacent
    // 256-bit regions. Other specializations reduce one region per group.
    const auto reduce = [input, storage, output]<class Vector,
        std::size_t regions_per_group = 1>
        [[msvc::noinline]] {
        const auto pack = [input] [[msvc::forceinline]] (
            const std::size_t bit) {
            static constexpr auto words_per_group =
                words_per_bit * regions_per_group;
            static constexpr auto vector_words = sizeof(Vector) / sizeof(T);
            static constexpr auto vector_count = words_per_group / vector_words;
            if constexpr ((vector_count == 1) ||
                byte_comparison_detail::kExpandBitmapReduction<
                    words_per_group * sizeof(T), Vector>) {
                const auto group = [input] [[msvc::forceinline]] (
                    const std::size_t start) {
                    if constexpr (vector_count == 1) {
                        return byte_comparison_detail::LoadVector<Vector>(
                            input + start * sizeof(T));
                    } else {
                        // A 4 MiB output bit covers at most 128 bitmap bytes
                        // when source units are 4 KiB or larger. Expanding
                        // these short groups removes the fixed-bound loops
                        // retained by MSVC; the pairwise OR tree also shortens
                        // their dependency chains.
                        return [input, start]<std::size_t... Index>
                            [[msvc::forceinline]] (
                            std::index_sequence<Index...>) {
                            return byte_comparison_detail::ReducePairwise<
                                byte_comparison_detail::VectorBitwiseOrInvoker>(
                                byte_comparison_detail::LoadVector<Vector>(
                                    input + start * sizeof(T) +
                                        Index * sizeof(Vector))...);
                        }(std::make_index_sequence<vector_count>{});
                    }
                };
                return [&group, bit]<std::size_t... Index>
                    [[msvc::forceinline]] (std::index_sequence<Index...>) {
                    if constexpr ((sizeof(Vector) == 64) &&
                        (regions_per_group == 1)) {
                        // Collecting the AVX-512 groups before packing is
                        // intended to reduce the repeated address calculations
                        // MSVC emits for direct arguments.
                        const auto values = std::array{
                            group((bit + Index * regions_per_group) *
                                words_per_bit)...};
                        return byte_comparison_detail::
                            ReduceVectorsToBitmapByte(values[Index]...);
                    } else {
                        // Passing groups directly avoids the pointer spills
                        // MSVC introduced when collecting short NEON groups
                        // into arrays or tuples.
                        return byte_comparison_detail::
                            ReduceVectorsToBitmapByte(
                                group((bit + Index * regions_per_group) *
                                    words_per_bit)...);
                    }
                }(std::make_index_sequence<
                    bits_per_byte / regions_per_group>{});
            } else {
                const auto groups = [input]<std::size_t... Group>
                    [[msvc::forceinline]] (
                    const std::size_t first, std::index_sequence<Group...>) {
                    auto values = std::array{
                        byte_comparison_detail::LoadVector<Vector>(
                            input + (first + Group * words_per_group) *
                                sizeof(T))...};
                    // The loop advances independent accumulators together,
                    // sharing loop control and allowing their loads and bitwise
                    // OR operations to overlap.
                    for (auto word = vector_words; word < words_per_group;
                         word += vector_words) {
                        ((values[Group] =
                            byte_comparison_detail::VectorBitwiseOr(
                                values[Group],
                                byte_comparison_detail::LoadVector<Vector>(
                                    input + (first + Group * words_per_group +
                                        word) * sizeof(T)))), ...);
                    }
                    return values;
                };
                constexpr auto regions_per_batch = bits_per_byte / 2;
                using Batch = std::make_index_sequence<
                    regions_per_batch / regions_per_group>;
                const auto low = groups(bit * words_per_bit, Batch{});
                const auto high = groups(
                    (bit + regions_per_batch) * words_per_bit, Batch{});
                return [&low, &high]<std::size_t... Index>
                    [[msvc::forceinline]] (std::index_sequence<Index...>) {
                    return byte_comparison_detail::ReduceVectorsToBitmapByte(
                        low[Index]..., high[Index]...);
                }(Batch{});
            }
        };
        auto bit = 0uz;
        const auto complete_bits = storage.size() / words_per_bit;
        for (; (complete_bits - bit) >= bits_per_byte; bit += bits_per_byte) {
            output[bit / bits_per_byte] = pack(bit);
        }
        // The last incomplete eight-region batch compares only the remaining
        // bytes. The result allocation initializes these tail bits to zero.
        for (auto first = bit * words_per_bit; first < storage.size();
             first += words_per_bit, ++bit) {
            const auto &byte = input[first * sizeof(T)];
            const auto size =
                std::min(words_per_bit, storage.size() - first) * sizeof(T);
            // A nonempty range is all zero if its first byte is zero and
            // each subsequent byte equals the preceding byte.
            if ((byte != 0) || (std::memcmp(&byte, &byte + 1, size - 1) != 0)) {
                output[bit / bits_per_byte] |= 1u << (bit % bits_per_byte);
            }
        }
    };
#ifdef _M_ARM64
    // At 128-bit width, the dispatcher selects NEON's expanded reductions
    // for their more compact packing.
    if (IsaSupport::operator()(PF_ARM_SVE_INSTRUCTIONS_AVAILABLE) &&
        (!byte_comparison_detail::kExpandBitmapReduction<
            words_per_bit * sizeof(T), uint8x16_t> ||
            (SveVectorBytes() > sizeof(uint8x16_t))) &&
        byte_comparison_detail::ReduceBitmapUsingSve<words_per_bit * sizeof(T)>(
            input, storage.size_bytes(), output)) {
        return result;
    }
    reduce.template operator()<uint8x16_t>();
#elifdef _M_X64
    if constexpr ((resolution_ratio == 256) || ((resolution_ratio % 512) == 0)) {
        if (IsaSupport::operator()(__IA_SUPPORT_VECTOR512, 0)) {
            reduce.template operator()<__m512i,
                (resolution_ratio == 256) ? 2 : 1>();
            return result;
        }
    }
    if constexpr ((resolution_ratio % 256) == 0) {
        if (IsaSupport::operator()(__IA_SUPPORT_VECTOR256, 0)) {
            reduce.template operator()<__m256i>();
            return result;
        }
    }
    reduce.template operator()<__m128i>();
#endif
    return result;
}

// Select a reduction for a source resolution of 512, 1024, 2048, 4096,
// 8192, 16384, or 32768 bytes. Other source resolutions return `std::nullopt`.
// `new_resolution` must be a multiple of 4 MiB so that all seven source
// resolutions satisfy the SIMD reduction's requirement that the number of
// source bits per output bit is a multiple of 128. The input and output
// contracts of `ReduceBitmap` also apply here.
export template <std::integral T,
    std::size_t new_resolution = kDefaultBitmapResolution,
    class IsaSupport = byte_comparison_detail::IsaSupportInvoker>
    requires ((new_resolution != 0) &&
        ((new_resolution % kDefaultBitmapResolution) == 0))
[[nodiscard]]
auto DispatchReduceBitmap(const std::span<T> storage,
    const std::size_t original_resolution,
    const std::size_t bitmap_minimum_length = 0)
    -> std::optional<ReducedBitmap> {
    const auto dispatch = [storage, original_resolution, bitmap_minimum_length]
        <std::size_t resolution_> [[msvc::forceinline]]
        (this const auto &self) -> std::optional<ReducedBitmap> {
        if (original_resolution == resolution_) {
            return ReduceBitmap<T, resolution_, new_resolution, IsaSupport>(
                storage, bitmap_minimum_length);
        }
        if constexpr (resolution_ < 32768) {
            return self.template operator()<resolution_ * 2>();
        }
        return std::nullopt;
    };
    return dispatch.template operator()<512>();
}

// Compile the 4 MiB dispatcher and its reductions here for byte-backed maps.
template auto DispatchReduceBitmap<const unsigned char>(
    std::span<const unsigned char>, std::size_t, std::size_t)
    -> std::optional<ReducedBitmap>;

} // namespace devicefs
