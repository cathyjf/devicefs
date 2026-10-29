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

auto devicefs::SveVectorBytes() noexcept -> std::size_t {
    return svcntb();
}

namespace devicefs::byte_comparison_detail {

// Load `bytes` bytes and combine the vectors using a balanced tree of bitwise
// OR operations. A predicated final load reads only the remaining bytes.
template <std::size_t bytes, std::size_t vector_bytes>
[[msvc::forceinline]]
auto LoadSveRegion(const unsigned char *const input) noexcept -> svuint8_t {
    if constexpr (bytes == vector_bytes) {
        return svld1_u8(svptrue_b8(), input);
    } else if constexpr (bytes < vector_bytes) {
        return svld1_u8(svwhilelt_b8_u64(0, bytes), input);
    } else {
        constexpr auto vectors = (bytes - 1) / vector_bytes + 1;
        constexpr auto split = (vectors / 2) * vector_bytes;
        return svorr_u8_x(svptrue_b8(),
            LoadSveRegion<split, vector_bytes>(input),
            LoadSveRegion<bytes - split, vector_bytes>(input + split));
    }
}

// Encode nonzero bytes from `count` consecutive accumulators using their
// source regions' bit positions. A horizontal bitwise OR of the returned
// vector produces those regions' bitmap bits.
template <std::size_t first, std::size_t count,
    std::size_t regions_per_accumulator>
[[msvc::forceinline]]
auto PackSveRegions(const auto values, const auto lane_shifts,
    const auto next_lane_shifts, const auto ones) noexcept {
    const auto all_lanes = svptrue_b8();
    if constexpr (count == 1) {
        // Reducing each byte to zero or one before shifting prevents its
        // original value from setting another source region's output bit.
        const auto flags = svmin_u8_x(all_lanes,
            svget4_u8(values, first), ones);
        if constexpr (regions_per_accumulator == 1) {
            if constexpr (first == 0) {
                return flags;
            } else {
                return svlsl_n_u8_x(all_lanes, flags, first);
            }
        } else {
            return svlsl_u8_x(all_lanes, flags,
                (first == 0) ? lane_shifts : next_lane_shifts);
        }
    } else {
        constexpr auto half = count / 2;
        return svorr_u8_x(all_lanes,
            PackSveRegions<first, half, regions_per_accumulator>(
                values, lane_shifts, next_lane_shifts, ones),
            PackSveRegions<first + half, half, regions_per_accumulator>(
                values, lane_shifts, next_lane_shifts, ones));
    }
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
    constexpr auto regions_per_batch = 4uz;
    // Each batch produces four output bits. Combining one, two, or four
    // adjacent regions per accumulator preserves that batch size while
    // allowing a wide load to supply more than one region.
    constexpr auto regions_per_accumulator = std::bit_floor(std::clamp(
        vector_bytes / source_bytes_per_output_bit, 1uz, regions_per_batch));
    constexpr auto bytes_per_accumulator =
        source_bytes_per_output_bit * regions_per_accumulator;
    constexpr auto accumulators_per_batch =
        regions_per_batch / regions_per_accumulator;
    const auto lane_shifts = []<std::size_t... Index> [[msvc::forceinline]] (
        std::index_sequence<Index...>) static noexcept {
        const auto all_lanes = svptrue_b8();
        const auto indices = svindex_u8(0, 1);
        std::conditional_t<sizeof...(Index) == 0, const svuint8_t, svuint8_t>
            shifts = svdup_n_u8(0);
        // Counting crossed region boundaries assigns each lane its region's
        // bit position, including when region sizes are not powers of two.
        ((shifts = svadd_n_u8_m(svcmpge_n_u8(all_lanes, indices,
            (Index + 1) * source_bytes_per_output_bit), shifts, 1)), ...);
        return shifts;
    }(std::make_index_sequence<regions_per_accumulator - 1>{});
    const auto reduce_regions = [input]<bool complete,
        std::size_t accumulator_bytes, std::size_t... Index>
        [[msvc::forceinline]] (const std::size_t first_byte,
        const std::size_t region_bytes,
        std::index_sequence<Index...>) noexcept {
        const auto zero = svdup_n_u8(0);
        auto region_bits = svcreate4_u8(zero, zero, zero, zero);
        constexpr auto vector_count =
            (accumulator_bytes - 1) / vector_bytes + 1;
        if constexpr (complete && (vector_count <= 4)) {
            // Expanding at most four loads per accumulator limits register
            // pressure while shortening the accumulation chains.
            ((region_bits = svset4_u8(region_bits, Index,
                LoadSveRegion<accumulator_bytes, vector_bytes>(
                    input + first_byte + Index * accumulator_bytes))), ...);
        } else {
            const auto all_lanes = svptrue_b8();
            // Larger batches advance independent accumulators together.
            // Tail calls use one accumulator and cap the region at the input end.
            for (auto offset = 0uz; offset < region_bytes; offset += vector_bytes) {
                const auto active_lanes = [offset, region_bytes] {
                    if constexpr (complete &&
                        ((accumulator_bytes % vector_bytes) == 0)) {
                        return svptrue_b8();
                    } else {
                        return svwhilelt_b8_u64(offset, region_bytes);
                    }
                }();
                // Inactive load lanes are zero and do not change the result.
                ((region_bits = svset4_u8(region_bits, Index,
                    svorr_u8_x(all_lanes, svget4_u8(region_bits, Index),
                        svld1_u8(active_lanes, input + first_byte +
                            Index * accumulator_bytes + offset)))), ...);
            }
        }
        return region_bits;
    };
    constexpr auto bits_per_byte = std::numeric_limits<unsigned char>::digits;
    static_assert(bits_per_byte == 8);
    const auto ones = svdup_n_u8(1);
    // Packing two regions per accumulator uses separate shift vectors for
    // bits 0 and 1, and bits 2 and 3.
    const auto next_lane_shifts = (regions_per_accumulator == 2)
        ? svadd_n_u8_x(svptrue_b8(), lane_shifts, regions_per_accumulator)
        : lane_shifts;
    auto bit = 0uz;
    const auto complete_regions = size / source_bytes_per_output_bit;
    for (; (complete_regions - bit) >= bits_per_byte; bit += bits_per_byte) {
        const auto low = PackSveRegions<0, accumulators_per_batch,
            regions_per_accumulator>(reduce_regions.template operator()<true,
                bytes_per_accumulator>(
                bit * source_bytes_per_output_bit, bytes_per_accumulator,
                std::make_index_sequence<accumulators_per_batch>{}),
                lane_shifts, next_lane_shifts, ones);
        const auto high = PackSveRegions<0, accumulators_per_batch,
            regions_per_accumulator>(reduce_regions.template operator()<true,
                bytes_per_accumulator>(
                (bit + regions_per_batch) * source_bytes_per_output_bit,
                bytes_per_accumulator,
                std::make_index_sequence<accumulators_per_batch>{}),
                lane_shifts, next_lane_shifts, ones);
        const auto all_lanes = svptrue_b8();
        output[bit / bits_per_byte] = svorv_u8(all_lanes,
            svorr_u8_x(all_lanes, low,
                svlsl_n_u8_x(all_lanes, high, regions_per_batch)));
    }
    for (auto first_byte = bit * source_bytes_per_output_bit; first_byte < size;
         first_byte += source_bytes_per_output_bit, ++bit) {
        const auto values = reduce_regions.template operator()<false,
            source_bytes_per_output_bit>(first_byte,
            std::min(source_bytes_per_output_bit, size - first_byte),
            std::make_index_sequence<1>{});
        if (svptest_any(svptrue_b8(),
                svcmpne_n_u8(svptrue_b8(), svget4_u8(values, 0), 0))) {
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
