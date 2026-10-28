// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module devicefs.byte_comparison;

import std;
import <devicefs/windows_imports.h>;

__pragma(warning(push))
__pragma(warning(disable : 5244, justification:
    "Including this header with C++ linkage attaches its symbols to the "
    "global module, not the named module. Including this header after the "
    "imports allows it to benefit from the symbols already defined in the "
    "main Windows header unit."))
extern "C++" {
#include <arm_sve.h>
}
__pragma(warning(pop))

namespace devicefs::byte_comparison_detail {

auto SveVectorBytes() noexcept -> std::size_t {
    return svcntb();
}

// Precondition: `vector_bytes == svcntb()`.
template <std::size_t source_bytes_per_output_bit, std::size_t vector_bytes>
[[msvc::noinline]]
[[gsl::suppress("26429", justification:
    "The `output` parameter is used for pointer arithmetic.")]]
auto ReduceBitmapUsingSveImpl(
    const unsigned char *const input,
    const std::size_t size,
    unsigned char *const output) noexcept -> void {
    const auto reduce_regions = [input]<std::size_t... Index>
        [[msvc::forceinline]] (const std::size_t first_byte,
        const std::size_t region_bytes,
        std::index_sequence<Index...>) noexcept {
        const auto all_lanes = svptrue_b8();
        const auto zero = svdup_n_u8(0);
        auto region_bits = svcreate4_u8(zero, zero, zero, zero);
        // Each tuple member accumulates one independent source region. This
        // loop handles four complete regions or one remaining region.
        for (auto offset = 0uz; offset < region_bytes; offset += vector_bytes) {
            const auto active_lanes = svwhilelt_b8_u64(offset, region_bytes);
            // Inactive load lanes are zero, so they do not change the OR.
            ((region_bits = svset4_u8(region_bits, Index,
                svorr_u8_x(all_lanes, svget4_u8(region_bits, Index),
                    svld1_u8(active_lanes, input + first_byte +
                        Index * source_bytes_per_output_bit + offset)))), ...);
        }
        return ((svptest_any(all_lanes,
            svcmpne_n_u8(all_lanes, svget4_u8(region_bits, Index), 0))
            << Index) | ...);
    };
    constexpr auto bits_per_byte = std::numeric_limits<unsigned char>::digits;
    static_assert(bits_per_byte == 8);
    constexpr auto regions_per_group = 4uz;
    auto bit = 0uz;
    const auto complete_regions = size / source_bytes_per_output_bit;
    for (; (complete_regions - bit) >= bits_per_byte; bit += bits_per_byte) {
        const auto low = reduce_regions(bit * source_bytes_per_output_bit,
            source_bytes_per_output_bit,
            std::make_index_sequence<regions_per_group>{});
        const auto high = reduce_regions(
            (bit + regions_per_group) * source_bytes_per_output_bit,
            source_bytes_per_output_bit,
            std::make_index_sequence<regions_per_group>{});
        __pragma(warning(push))
        __pragma(warning(disable : 4244, justification :
            "Each mask is in [0, 15]. Combining the low mask with the "
            "high mask shifted left by four produces a value in [0, 255], "
            "which fits in the output byte."))
        output[bit / bits_per_byte] = low | (high << regions_per_group);
        __pragma(warning(pop))
    }
    for (auto first_byte = bit * source_bytes_per_output_bit; first_byte < size;
         first_byte += source_bytes_per_output_bit, ++bit) {
        if (reduce_regions(first_byte,
                std::min(source_bytes_per_output_bit, size - first_byte),
                std::make_index_sequence<1>{})) {
            output[bit / bits_per_byte] |= 1u << (bit % bits_per_byte);
        }
    }
}

// Dispatch over SVE vector byte widths in [16, 256] in 16-byte steps.
template <std::size_t source_bytes_per_output_bit>
_Use_decl_annotations_
auto ReduceBitmapUsingSve(
    const unsigned char *const input,
    const std::size_t size,
    unsigned char *const output) noexcept -> bool {
    const auto dispatch = [input, size, output, vector_bytes = svcntb()]
        <std::size_t width> [[msvc::forceinline]]
        (this const auto &self) noexcept -> bool {
        if (vector_bytes == width) {
            ReduceBitmapUsingSveImpl<source_bytes_per_output_bit, width>(
                input, size, output);
            return true;
        }
        if constexpr (width < 256) {
            return self.template operator()<width + 16>();
        } else {
            return false;
        }
    };
    return dispatch.template operator()<16>();
}

#define DEVICEFS_INSTANTIATE_SVE_REDUCTION(Bytes) \
    template auto ReduceBitmapUsingSve<Bytes>( \
        const unsigned char *, std::size_t, unsigned char *) noexcept -> bool

#include "byte_comparison_sve.h"

} // namespace devicefs::byte_comparison_detail
