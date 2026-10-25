// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.byte_comparison;

import std;
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
#if defined(_M_ARM64)
    auto count = 0uz;
    auto offset = 0uz;
    constexpr auto width = sizeof(uint8x16_t);
    for (; (size - offset) >= width; offset += width) {
        const auto equal = vceqq_u8(
            vld1q_u8(left + offset), vld1q_u8(right + offset));
        // The inverted comparison contains `0xff` for each unequal byte.
        // Shifting those values to 1 keeps their sum within a byte.
        count += vaddvq_u8(vshrq_n_u8(vmvnq_u8(equal), 7));
    }
#elif defined(_M_X64)
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
        const auto load = []<class Byte> [[msvc::forceinline]] (
            const Byte *const input) static noexcept {
            if constexpr (std::same_as<Vector, __m512i>) {
                return _mm512_loadu_si512(input);
            } else {
                using Bytes = Byte[sizeof(Vector)];
                // The `std::bit_cast` function does not require a live object
                // of the source type. It requires only that each bit within the
                // representation of the source be within some live object.
                // Here, that requirement is met because each bit is within
                // a live `unsigned char` object.
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
        };
        auto offset = 0uz;
        auto totals = zero;
        for (; (size - offset) >= width; offset += width) {
            const auto left_bytes = load(left + offset);
            const auto right_bytes = load(right + offset);
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

} // namespace devicefs
