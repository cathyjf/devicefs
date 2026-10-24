// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.byte_comparison;

import std;
import <devicefs/windows_imports.h>;

#ifdef _M_ARM64
extern "C++" {
// Including this header after the imports allows it to benefit from the
// symbols already defined in the main Windows header unit.
#include <arm64_neon.h>
}
#endif

namespace devicefs {

// Count the byte positions at which the two buffers differ.
export
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
    auto offset = 0uz;
    auto count = 0uz;
#if defined(_M_ARM64)
    constexpr auto width = sizeof(uint8x16_t);
    for (; (size - offset) >= width; offset += width) {
        const auto equal = vceqq_u8(
            vld1q_u8(left + offset), vld1q_u8(right + offset));
        // The inverted comparison contains `0xff` for each unequal byte.
        // Shifting those values to 1 keeps their sum within a byte.
        count += vaddvq_u8(vshrq_n_u8(vmvnq_u8(equal), 7));
    }
#elif defined(_M_X64)
    constexpr auto width = sizeof(__m128i);
    const auto zero = _mm_setzero_si128();
    const auto one = _mm_set1_epi8(1);
    auto totals = zero;
    for (; (size - offset) >= width; offset += width) {
        /* [[indeterminate]] */ __m128i left_bytes;
        /* [[indeterminate]] */ __m128i right_bytes;
        std::memcpy(&left_bytes, left + offset, width);
        std::memcpy(&right_bytes, right + offset, width);
        const auto different = _mm_andnot_si128(
            _mm_cmpeq_epi8(left_bytes, right_bytes), one);
        // `_mm_sad_epu8` sums each group of eight bytes into a 64-bit
        // lane. Since `different` contains only 0 and 1, each lane's
        // total is at most `size`.
        totals = _mm_add_epi64(totals, _mm_sad_epu8(different, zero));
    }
    const auto combined = _mm_add_epi64(
        totals, _mm_srli_si128(totals, sizeof(std::uint64_t)));
    // `_mm_cvtsi128_si64` returns a signed integer. This expression interprets
    // its bits as the unsigned count so values above `INT64_MAX` remain valid.
    count = std::bit_cast<std::size_t>(_mm_cvtsi128_si64(combined));
#else
#error `CountDifferingBytes` lacks an implementation for this architecture
#endif
    for (auto index = 0uz; index < (size % width); ++index) {
        count += left[offset + index] != right[offset + index];
    }
    return count;
}

} // namespace devicefs
