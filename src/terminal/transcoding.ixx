// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <simdutf/simdutf.h>
#include "compat/gsl_suppress.h"

export module devicefs.terminal.transcoding;

import std;

namespace devicefs::terminal::detail {

template <typename Character>
concept UtfCharacter = std::same_as<Character, char> || std::same_as<Character, char8_t> ||
    std::same_as<Character, char16_t> || std::same_as<Character, char32_t>;

// simdutf writes into caller-owned storage. UTF-8 input needs at most one output
// code unit per byte. UTF-16 to UTF-32 also cannot grow in code-unit count.
// Conversions that can grow use simdutf's length calculation before allocating.
template <UtfCharacter Output, UtfCharacter Input>
[[nodiscard]] constexpr auto TranscodedCapacity(const std::basic_string_view<Input> input) noexcept {
    if constexpr (sizeof(Output) == 1 && sizeof(Input) == 2) {
        return simdutf::utf8_length_from_utf16(std::span{input});
    } else if constexpr (sizeof(Output) == 1 && sizeof(Input) == 4) {
        return simdutf::utf8_length_from_utf32(std::span{input});
    } else if constexpr (sizeof(Output) == 2 && sizeof(Input) == 4) {
        return simdutf::utf16_length_from_utf32(std::span{input});
    } else {
        return input.size();
    }
}

template <UtfCharacter Output, UtfCharacter Input>
[[nodiscard]] auto ConvertText(const std::basic_string_view<Input> input,
    const std::span<Output> output) -> std::size_t {
    const auto result = [&] {
        if constexpr (sizeof(Input) == 1 && sizeof(Output) == 2) {
            return simdutf::convert_utf8_to_utf16_with_errors(std::span{input}, output);
        } else if constexpr (sizeof(Input) == 1 && sizeof(Output) == 4) {
            return simdutf::convert_utf8_to_utf32_with_errors(std::span{input}, output);
        } else if constexpr (sizeof(Input) == 2 && sizeof(Output) == 1) {
            return simdutf::convert_utf16_to_utf8_with_errors(std::span{input}, output);
        } else if constexpr (sizeof(Input) == 2 && sizeof(Output) == 4) {
            return simdutf::convert_utf16_to_utf32_with_errors(std::span{input}, output);
        } else if constexpr (sizeof(Input) == 4 && sizeof(Output) == 1) {
            return simdutf::convert_utf32_to_utf8_with_errors(std::span{input}, output);
        } else if constexpr (sizeof(Input) == 4 && sizeof(Output) == 2) {
            return simdutf::convert_utf32_to_utf16_with_errors(std::span{input}, output);
        } else {
            const auto validated = [&] {
                if constexpr (sizeof(Input) == 1) {
                    return simdutf::validate_utf8_with_errors(std::span{input});
                } else if constexpr (sizeof(Input) == 2) {
                    return simdutf::validate_utf16_with_errors(std::span{input});
                } else {
                    return simdutf::validate_utf32_with_errors(std::span{input});
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

export namespace devicefs::terminal {

// `TranscodedText` owns null-terminated Unicode text. `size()` excludes the
// terminator and includes embedded NULs. A conversion to `basic_string_view`
// borrows the result's storage, which remains valid until the owner is moved or
// destroyed. Results are movable; a string constructed from the view copies the
// text. A moved-from result contains an empty, null-terminated string.
// Conversions whose capacity estimate fits 256 code units, including the final
// NUL, use inline storage. Larger conversions allocate their buffer on the heap.
template <detail::UtfCharacter Character>
class TranscodedText {
public:
    template <detail::UtfCharacter Input>
    GSL_SUPPRESS("26495",
        "Conversion initializes the output prefix and its terminator before any "
        "read. The unused tail of `inline_` is never read, including when moving.")
    explicit TranscodedText(const std::basic_string_view<Input> input) {
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
// string into an owning `TranscodedText<Output>`. `char` and `char8_t` mean UTF-8;
// `char16_t` and `char32_t` mean native-endian UTF-16 and UTF-32. Conversion is
// independent of the locale. Malformed input throws `std::invalid_argument`
// identifying the offending code-unit offset. String views and strings preserve
// embedded NULs; pointers and literals end at their first NUL.
template <detail::UtfCharacter Output>
[[nodiscard]] auto Transcode(const auto &text) {
    return TranscodedText<Output>{std::basic_string_view{text}};
}

}
