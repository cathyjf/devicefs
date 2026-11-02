// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.crc32;

import std;
import <devicefs/common.h>;
import <devicefs/windows_imports.h>;

#ifdef _M_X64
import <isa_availability.h>;
#endif

namespace devicefs::crc32_detail {

#ifdef _M_ARM64
constexpr auto kIsArm64 = true;
#else
constexpr auto kIsArm64 = false;
#endif

constexpr auto kCastagnoliPolynomial = std::uint32_t{0x82f63b78};
constexpr auto kIeeePolynomial = std::uint32_t{0xedb88320};

#define DEVICEFS_CRC_INTRINSIC(Name, ArmIntrinsic, X64Intrinsic) \
    struct Name { \
        [[msvc::forceinline]] \
        static auto operator()(const auto checksum, \
            const auto value) noexcept { \
            if constexpr (kIsArm64) { \
                return ArmIntrinsic(checksum, value); \
            } else { \
                return X64Intrinsic(checksum, value); \
            } \
        } \
    }

auto Unimplemented(auto...) { std::unreachable(); }

struct CastagnoliInstructions {
    DEVICEFS_CRC_INTRINSIC(Word, __crc32cd, _mm_crc32_u64);
    DEVICEFS_CRC_INTRINSIC(Byte, __crc32cb, _mm_crc32_u8);
};

struct IeeeInstructions {
    DEVICEFS_CRC_INTRINSIC(Word, __crc32d, Unimplemented);
    DEVICEFS_CRC_INTRINSIC(Byte, __crc32b, Unimplemented);
};

#undef DEVICEFS_CRC_INTRINSIC

template <std::uint32_t Polynomial>
[[msvc::forceinline]]
auto UpdateCrc32(const std::uint32_t checksum, const auto value) noexcept
    -> std::uint32_t {
    using Instructions = std::conditional_t<
        Polynomial == kCastagnoliPolynomial,
        CastagnoliInstructions, IeeeInstructions>;
    using Instruction = std::conditional_t<
        sizeof(value) == sizeof(std::uint64_t),
        typename Instructions::Word, typename Instructions::Byte>;
    [[gsl::suppress("26472", justification:
        "The upper 32 bits of `_mm_crc32_u64`'s result are zero, so "
        "converting that result to `std::uint32_t` preserves its value.")]]
    return static_cast<std::uint32_t>(
        Instruction::operator()(checksum, value));
}

template <std::uint32_t Polynomial>
[[nodiscard, msvc::noinline]]
auto Crc32UsingInstructions(const std::span<const std::byte> input) noexcept
    -> std::uint32_t {
    auto result = std::numeric_limits<std::uint32_t>::max();
    auto offset = 0uz;
    for (; (input.size() - offset) >= sizeof(std::uint64_t);
            offset += sizeof(std::uint64_t)) {
        result = UpdateCrc32<Polynomial>(result,
            BitPrefixCast<std::uint64_t>(input[offset]));
    }
    for (; offset < input.size(); ++offset) {
        result = UpdateCrc32<Polynomial>(result,
            std::to_integer<std::uint8_t>(input[offset]));
    }
    return ~result;
}

[[nodiscard, msvc::forceinline]]
constexpr auto CanUseCrc32Instructions() noexcept -> bool {
    if consteval {
        return false;
    }
#ifdef _M_ARM64
    return IsProcessorFeaturePresent(PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE);
#elifdef _M_X64
    return __check_isa_support(__IA_SUPPORT_SSE42, 0);
#endif
}

extern "C" NTSYSAPI ULONG32 NTAPI RtlComputeCrc32(
    _In_ ULONG32 partial_crc, _In_ const void *buffer, _In_ ULONG length);

[[nodiscard, msvc::forceinline]]
auto Crc32UsingNtApi(const std::span<const std::byte> input) noexcept {
    auto result = std::uint32_t{};
    constexpr auto chunk_size =
        CompileTimeCast<std::size_t, std::numeric_limits<ULONG>::max()>();
    for (auto i = 0uz; i < input.size(); i += chunk_size) {
        const auto length = static_cast<ULONG>(
            std::min(chunk_size, input.size() - i));
        result = RtlComputeCrc32(result, input.data() + i, length);
    }
    return result;
}

} // namespace devicefs::crc32_detail

namespace devicefs {

// Compute CRC-32 (IEEE) unless another reflected polynomial is specified.
export template <std::uint32_t Polynomial = crc32_detail::kIeeePolynomial>
[[nodiscard]] constexpr auto Crc32(
    const std::span<const std::byte> input) noexcept -> std::uint32_t {
    if constexpr ((Polynomial == crc32_detail::kCastagnoliPolynomial) ||
        (crc32_detail::kIsArm64 &&
            (Polynomial == crc32_detail::kIeeePolynomial))) {
        if (crc32_detail::CanUseCrc32Instructions()) {
            return crc32_detail::Crc32UsingInstructions<Polynomial>(input);
        }
    } else if constexpr (!crc32_detail::kIsArm64 &&
        (Polynomial == crc32_detail::kIeeePolynomial)) {
        if not consteval {
            return crc32_detail::Crc32UsingNtApi(input);
        }
    }
    auto result = std::numeric_limits<std::uint32_t>::max();
    for (const auto byte : input) {
        result ^= std::to_integer<std::uint32_t>(byte);
        for (auto bit = 0; bit < 8; ++bit) {
            const auto mask = std::uint32_t{} - (result & 1u);
            result = (result >> 1) ^ (Polynomial & mask);
        }
    }
    return ~result;
}

// Compute CRC-32C (Castagnoli).
export [[nodiscard]] constexpr auto Crc32c(
    const std::span<const std::byte> input) noexcept {
    return Crc32<crc32_detail::kCastagnoliPolynomial>(input);
}

} // namespace devicefs
