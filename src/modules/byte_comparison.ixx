// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.byte_comparison;

import std;
import devicefs.bitmap_set;
import <devicefs/common.h>;
import <devicefs/windows_imports.h>;

#ifdef _M_X64
import <isa_availability.h>;
#endif

#ifdef _M_ARM64
extern "C++" {
// Including this header after the imports allows it to benefit from the
// symbols already defined in the main Windows header unit.
#include <arm64_neon.h>
}
#endif

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
    if constexpr (std::same_as<Vector, __m256i>) {
        return _mm256_or_si256(left, right);
    } else {
        return _mm_or_si128(left, right);
    }
#else
#error `VectorBitwiseOr` lacks an implementation for this architecture
#endif
}

// Return four zero-or-one bytes in source order, one per vector. A byte
// is one if any bit in its vector is set.
template <class Vector>
[[msvc::forceinline]]
auto ReduceVectorsToBooleanBytes(const Vector a, const Vector b,
    const Vector c, const Vector d) noexcept {
#ifdef _M_ARM64
    // Pairwise maxima reduce each vector to one byte while preserving the
    // order of the four groups. A maximum is nonzero if any input bit is set.
    const auto ab = vpmaxq_u8(a, b);
    const auto cd = vpmaxq_u8(c, d);
    const auto quarters = vpmaxq_u8(ab, cd);
    const auto pairs = vget_low_u8(vpmaxq_u8(quarters, quarters));
    const auto maxima = vpmax_u8(pairs, pairs);
    const auto masks = vcgt_u8(maxima, vdup_n_u8(0));
    const auto flags = vget_lane_u32(vreinterpret_u32_u8(
        vand_u8(masks, vdup_n_u8(1))), 0);
#elifdef _M_X64
    const auto groups = [a, b, c, d] [[msvc::forceinline]] {
        if constexpr (std::same_as<Vector, __m256i>) {
            const auto ab = _mm256_or_si256(
                _mm256_unpacklo_epi32(a, b),
                _mm256_unpackhi_epi32(a, b));
            const auto cd = _mm256_or_si256(
                _mm256_unpacklo_epi32(c, d),
                _mm256_unpackhi_epi32(c, d));
            const auto halves = _mm256_or_si256(
                _mm256_unpacklo_epi64(ab, cd),
                _mm256_unpackhi_epi64(ab, cd));
            return _mm_or_si128(_mm256_castsi256_si128(halves),
                _mm256_extracti128_si256(halves, 1));
        } else {
            const auto ab = _mm_or_si128(_mm_unpacklo_epi32(a, b),
                _mm_unpackhi_epi32(a, b));
            const auto cd = _mm_or_si128(_mm_unpacklo_epi32(c, d),
                _mm_unpackhi_epi32(c, d));
            return _mm_or_si128(_mm_unpacklo_epi64(ab, cd),
                _mm_unpackhi_epi64(ab, cd));
        }
    }();
    const auto zero = _mm_setzero_si128();
    const auto masks = _mm_cmpeq_epi32(groups, zero);
    const auto narrow = _mm_packs_epi16(
        _mm_packs_epi32(masks, zero), zero);
    const auto flags = _mm_cvtsi128_si32(
        _mm_andnot_si128(narrow, _mm_set1_epi8(1)));
#else
#error `ReduceVectorsToBooleanBytes` lacks an implementation for this architecture
#endif
    return flags;
}

// Pack `sizeof(Vector)` zero-or-one flag bytes into consecutive bitmap bits.
template <class Vector>
[[msvc::forceinline]]
auto PackBooleanBytesIntoBitmap(const ByteType auto *const input) noexcept {
    const auto flags = LoadVector<Vector>(input);
#ifdef _M_ARM64
    static constexpr auto weights = std::to_array<unsigned char>({
        1, 2, 4, 8, 16, 32, 64, 128,
        1, 2, 4, 8, 16, 32, 64, 128});
    const auto weighted = vmulq_u8(flags,
        vld1q_u8(weights.data()));
    return std::array{vaddv_u8(vget_low_u8(weighted)),
        vaddv_u8(vget_high_u8(weighted))};
#elifdef _M_X64
    const auto mask = [flags] [[msvc::forceinline]] {
        if constexpr (std::same_as<Vector, __m256i>) {
            return _mm256_movemask_epi8(_mm256_slli_epi16(flags, 7));
        } else {
            return _mm_movemask_epi8(_mm_slli_epi16(flags, 7));
        }
    }();
    return std::bit_cast<std::array<char, sizeof(mask)>>(mask);
#else
#error `PackBooleanBytesIntoBitmap` lacks an implementation for this architecture
#endif
}

struct IsaSupportInvoker {
    [[msvc::forceinline]]
    static auto operator()(auto &&...arguments) noexcept {
        return __check_isa_support(
            std::forward<decltype(arguments)>(arguments)...);
    }
};

} // namespace devicefs::byte_comparison_detail

namespace devicefs {

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

// Reduce a bitmap whose bits each describe `original_resolution` volume bytes
// to one whose bits each describe `new_resolution` bytes. Both maps use the
// low bit of each byte first. Every bit in `storage` participates in the
// reduction. A partial final group produces one output bit.
// The result contains at least `bitmap_minimum_length` bytes. Bits and bytes
// beyond the reduced bitmap have unspecified values.
// Precondition: `storage` is nonempty.
export template <std::integral T,
    class IsaSupport = byte_comparison_detail::IsaSupportInvoker>
[[nodiscard]]
_Pre_satisfies_(original_resolution != 0)
_Pre_satisfies_(new_resolution != 0)
_Pre_satisfies_((new_resolution % original_resolution) == 0)
_Pre_satisfies_(((new_resolution / original_resolution) % 128) == 0)
auto ReduceBitmap(const std::span<T> storage,
    const std::size_t original_resolution, const std::size_t new_resolution,
    const std::size_t bitmap_minimum_length = 0)
    -> ReallocVector<unsigned char> {
    constexpr auto bits_per_byte = std::numeric_limits<unsigned char>::digits;
    static_assert(bits_per_byte == 8);
    static_assert(std::endian::native == std::endian::little);
    const auto resolution_ratio = new_resolution / original_resolution;
    const auto words_per_bit =
        resolution_ratio / (sizeof(T) * bits_per_byte);
    const auto bits = storage.size() / words_per_bit +
        ((storage.size() % words_per_bit) != 0);
    const auto output_size =
        bits / bits_per_byte + ((bits % bits_per_byte) != 0);
    // The suffix holds independent flags. Padding to 32 bytes permits a full
    // AVX2 load even for the final partial batch of output bits.
    constexpr auto flag_batch_size = 32uz;
    const auto flag_bytes = (bits / flag_batch_size +
        ((bits % flag_batch_size) != 0)) * flag_batch_size;

    auto result = ReallocVector<unsigned char>{};
    result.resize(std::max(output_size + flag_bytes, bitmap_minimum_length));
    auto *const output = std::addressof(result[0]);
    auto *const intermediate = output + output_size;
    const auto *const input =
        ByteViewCast<const unsigned char *>(storage.data());

    const auto reduce = [input, storage, output, intermediate,
        output_size, words_per_bit]<class Vector> [[msvc::noinline]] {
        const auto group = [input, words_per_bit] [[msvc::forceinline]] (
            const std::size_t first) {
            constexpr auto vector_words = sizeof(Vector) / sizeof(T);
            auto value = byte_comparison_detail::LoadVector<Vector>(
                input + first * sizeof(T));
            for (auto word = vector_words; word < words_per_bit;
                 word += vector_words) {
                const auto next = byte_comparison_detail::LoadVector<Vector>(
                    input + (first + word) * sizeof(T));
                value = byte_comparison_detail::VectorBitwiseOr(value, next);
            }
            return value;
        };
        auto bit = 0uz;
        const auto complete_bits = storage.size() / words_per_bit;
        for (; (complete_bits - bit) >= 4; bit += 4) {
            const auto a = group(bit * words_per_bit);
            const auto b = group((bit + 1) * words_per_bit);
            const auto c = group((bit + 2) * words_per_bit);
            const auto d = group((bit + 3) * words_per_bit);
            const auto flags =
                byte_comparison_detail::ReduceVectorsToBooleanBytes(a, b, c, d);
            std::memcpy(intermediate + bit, &flags, sizeof(flags));
        }
        // The last incomplete four-region batch compares only the remaining bytes.
        for (auto first = bit * words_per_bit; first < storage.size();
             first += words_per_bit, ++bit) {
            const auto *const bytes = input + first * sizeof(T);
            const auto size =
                std::min(words_per_bit, storage.size() - first) * sizeof(T);
            // A nonempty range is all zero if its first byte is zero and
            // each subsequent byte equals the preceding byte.
            intermediate[bit] = (*bytes != 0) ||
                (std::memcmp(bytes, bytes + 1, size - 1) != 0);
        }

        constexpr auto packed_bytes = sizeof(Vector) / bits_per_byte;
        auto byte = 0uz;
        for (; (output_size - byte) >= packed_bytes; byte += packed_bytes) {
            const auto packed =
                byte_comparison_detail::PackBooleanBytesIntoBitmap<Vector>(
                    intermediate + byte * bits_per_byte);
            std::memcpy(output + byte, packed.data(), packed_bytes);
        }
        if (byte != output_size) {
            const auto packed =
                byte_comparison_detail::PackBooleanBytesIntoBitmap<Vector>(
                    intermediate + byte * bits_per_byte);
            std::memcpy(output + byte, packed.data(), output_size - byte);
        }
    };
#ifdef _M_ARM64
    reduce.template operator()<uint8x16_t>();
#elifdef _M_X64
    if (((resolution_ratio % 256) == 0) &&
        IsaSupport::operator()(__IA_SUPPORT_VECTOR256, 0)) {
        reduce.template operator()<__m256i>();
    } else {
        reduce.template operator()<__m128i>();
    }
#endif
    result.resize(std::max(output_size, bitmap_minimum_length));
    return result;
}

template auto ReduceBitmap<>(std::span<const std::uint64_t>,
    std::size_t, std::size_t, std::size_t) -> ReallocVector<unsigned char>;

} // namespace devicefs
