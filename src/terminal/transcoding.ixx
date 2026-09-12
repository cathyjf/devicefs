// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include "compat/gsl_suppress.h"
#include "compat/forceinline_compat.h"

#ifndef _MSC_VER
    // MSBuild provides automatic support for header units, so the simdutf code
    // can be (and is) imported as a header unit below. However, aside from the
    // Windows build (which benefits from the MSBuild automation), CMake does
    // not support compiling header units. As a result, on non-Windows
    // platforms, we must textually include the simdutf code.
    //
    // simdutf's inline functions call helpers in anonymous namespaces. Those
    // references are forbidden in the named part of a module interface, so the
    // textual inclusion of the combined declarations and implementation
    // must occur in the global module fragment.
    // https://eel.is/c++draft/basic.link#17
    #include "simdutf/combined.h"
#endif

export module devicefs.terminal.transcoding;

import std;

#ifdef _MSC_VER
    import <simdutf/combined.h>;
#endif

namespace devicefs::terminal::detail {

template <typename Character>
concept UtfCharacter = std::same_as<Character, char> || std::same_as<Character, char8_t> ||
    std::same_as<Character, char16_t> || std::same_as<Character, char32_t> ||
    std::same_as<Character, wchar_t>;

// Our simdutf adaptation uses `wchar_t` for the matching encoding.
// The other C++ character type of that width needs a value-copy adapter;
// native wide strings are passed directly to simdutf.
template <typename Character>
using SimdUnit = std::conditional_t<sizeof(Character) == 2, utf16_code_unit,
    std::conditional_t<sizeof(Character) == 4, utf32_code_unit, Character>>;

template <typename Allocator, typename Character>
using ReboundString = std::basic_string<Character, std::char_traits<Character>,
    typename std::allocator_traits<Allocator>::template rebind_alloc<Character>>;

template <typename Allocator, UtfCharacter Input>
[[nodiscard]] auto UtfInput(const std::basic_string_view<Input> input)
    noexcept(std::same_as<Input, SimdUnit<Input>>) {
    if constexpr (!std::same_as<Input, SimdUnit<Input>>) {
        return ReboundString<Allocator, SimdUnit<Input>>(input.begin(), input.end());
    } else {
        return input;
    }
}

// simdutf writes into caller-owned storage. UTF-8 input needs at most one output
// code unit per byte. UTF-16 to UTF-32 also cannot grow in code-unit count.
// Conversions that can grow use simdutf's length calculation before allocating.
template <UtfCharacter Output, UtfCharacter Input>
[[nodiscard]] constexpr auto TranscodedCapacity(const std::basic_string_view<Input> input) noexcept {
    if constexpr (sizeof(Output) == 1 && sizeof(Input) == 2) {
        return simdutf::utf8_length_from_utf16(input);
    } else if constexpr (sizeof(Output) == 1 && sizeof(Input) == 4) {
        return simdutf::utf8_length_from_utf32(input);
    } else if constexpr (sizeof(Output) == 2 && sizeof(Input) == 4) {
        return simdutf::utf16_length_from_utf32(input);
    } else {
        return input.size();
    }
}

template <typename Allocator = std::allocator<std::byte>, UtfCharacter Output, UtfCharacter Input>
[[nodiscard]] auto ConvertText(const std::basic_string_view<Input> input,
    const std::span<Output> output) -> std::size_t {
    if constexpr (!std::same_as<Output, SimdUnit<Output>>) {
        auto units = ReboundString<Allocator, SimdUnit<Output>>(output.size(), SimdUnit<Output>{});
        const auto count = ConvertText(input, std::span{units});
        std::copy_n(units.begin(), count, output.begin());
        return count;
    } else {
        const auto result = [&] {
            if constexpr (sizeof(Input) == 1 && sizeof(Output) == 2) {
                return simdutf::convert_utf8_to_utf16_with_errors(input, output);
            } else if constexpr (sizeof(Input) == 1 && sizeof(Output) == 4) {
                return simdutf::convert_utf8_to_utf32_with_errors(input, output);
            } else if constexpr (sizeof(Input) == 2 && sizeof(Output) == 1) {
                return simdutf::convert_utf16_to_utf8_with_errors(input, output);
            } else if constexpr (sizeof(Input) == 2 && sizeof(Output) == 4) {
                return simdutf::convert_utf16_to_utf32_with_errors(input, output);
            } else if constexpr (sizeof(Input) == 4 && sizeof(Output) == 1) {
                return simdutf::convert_utf32_to_utf8_with_errors(input, output);
            } else if constexpr (sizeof(Input) == 4 && sizeof(Output) == 2) {
                return simdutf::convert_utf32_to_utf16_with_errors(input, output);
            } else {
                const auto validated = [&] {
                    if constexpr (sizeof(Input) == 1) {
                        return simdutf::validate_utf8_with_errors(input);
                    } else if constexpr (sizeof(Input) == 2) {
                        return simdutf::validate_utf16_with_errors(input);
                    } else {
                        return simdutf::validate_utf32_with_errors(input);
                    }
                }();
                if (validated.error == simdutf::SUCCESS) {
                    std::memcpy(output.data(), input.data(), input.size() * sizeof(Input));
                }
                return validated;
            }
        }();
        if (result.error != simdutf::SUCCESS) {
            throw std::invalid_argument(std::format(
                "could not transcode UTF-{} input at code-unit offset {}: {}",
                sizeof(Input) * 8, result.count, simdutf::error_to_string(result.error)));
        }
        return result.count;
    }
}

}

export namespace devicefs::terminal {

// `TranscodedText` owns null-terminated Unicode text. `size()` excludes the
// terminator and includes embedded NULs. A conversion to `basic_string_view`
// borrows the result's storage, which remains valid until the owner is moved or
// destroyed. Results are movable; a string constructed from the view copies the
// text. A moved-from result contains an empty, null-terminated string.
// Results whose capacity estimate fits 256 code units, including the final
// NUL, use inline storage. Larger results allocate their buffer on the heap.
template <detail::UtfCharacter Character>
class TranscodedText {
public:
    template <detail::UtfCharacter Input>
    GSL_SUPPRESS("26495",
        "Conversion initializes the output prefix and its terminator before any "
        "read. The unused tail of `inline_` is never read, including when moving.")
    explicit TranscodedText(const std::basic_string_view<Input> text) {
        const auto units = detail::UtfInput<std::allocator<Character>>(text);
        const auto input = std::basic_string_view{units};
        const auto capacity = input.empty() ? 0 : detail::TranscodedCapacity<Character>(input);
        if (capacity >= (std::numeric_limits<std::size_t>::max() / sizeof(Character))) {
            throw std::length_error("transcoded text and its terminator exceed the allocation limit");
        }
        if (capacity >= inline_.size()) {
            heap_ = std::make_unique_for_overwrite<Character[]>(capacity + 1);
        }
        const auto output = std::span{heap_ ? heap_.get() : inline_.data(), capacity + 1};
        size_ = input.empty() ? 0 : detail::ConvertText(input, output);
        output[size_] = Character{};
    }

    TranscodedText(const TranscodedText &) = delete;
    auto operator=(const TranscodedText &) -> TranscodedText & = delete;
    auto operator=(TranscodedText &&) -> TranscodedText & = delete;
    ~TranscodedText() = default;

    GSL_SUPPRESS("26495",
        "Moving copies only the initialized text and terminator, or transfers "
        "the heap buffer. All reads use that prefix; unused `inline_` elements "
        "remain uninitialized.")
    TranscodedText(TranscodedText &&other) noexcept
        : heap_{std::move(other.heap_)}, size_{std::exchange(other.size_, 0)} {
        if (!heap_) {
            std::copy_n(other.inline_.begin(), size_ + 1, inline_.begin());
        }
        other.inline_[0] = Character{};
    }

    [[nodiscard]] auto data() const noexcept -> const Character * {
        return heap_ ? heap_.get() : inline_.data();
    }

    [[nodiscard]] auto size() const noexcept { return size_; }
    operator std::basic_string_view<Character>() const noexcept { return {data(), size()}; }

private:
    std::array<Character, 256> inline_;
    std::unique_ptr<Character[]> heap_;
    std::size_t size_;
};

// `Transcode<Output>(text)` converts a string, string view, or null-terminated
// string. A character type as `Output` returns an owning `TranscodedText<Output>`;
// a `basic_string` type returns that string type. `char` and `char8_t` mean UTF-8;
// `char16_t` and `char32_t` mean native-endian UTF-16 and UTF-32. Conversion is
// independent of the locale. Malformed input throws `std::invalid_argument`
// identifying the offending code-unit offset. String views and strings preserve
// embedded NULs; pointers and literals end at their first NUL.
// A 16-bit `wchar_t` means UTF-16; a 32-bit `wchar_t` means UTF-32.
// For a `basic_string` result, any temporary code-unit buffers use that string
// type's allocator, including for secure allocators.
template <typename Output>
    requires detail::UtfCharacter<Output> || requires { typename Output::value_type; }
[[nodiscard]] auto Transcode(const auto &text) {
    const auto input = std::basic_string_view{text};
    // Recursive inlining removes constructor and simdutf implementation-accessor
    // calls that survive ordinary inlining in MSVC x64 builds. These calls add
    // overhead to short conversions even when the result fits in inline storage.
    ATTRIBUTE_MSVC_FLATTEN
    if constexpr (detail::UtfCharacter<Output>) {
        return TranscodedText<Output>{input};
    } else {
        using Character = typename Output::value_type;
        using Allocator = typename Output::allocator_type;
        const auto units = detail::UtfInput<Allocator>(input);
        const auto source = std::basic_string_view{units};
        const auto capacity = source.empty() ? 0 : detail::TranscodedCapacity<Character>(source);
        auto result = Output(capacity, Character{});
        if (!source.empty()) {
            result.resize(detail::ConvertText<Allocator>(source, std::span{result}));
        }
        return result;
    }
}

}
