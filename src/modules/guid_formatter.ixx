// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.guid_formatter;

import std;
import <devicefs/common.h>;
import <devicefs/windows_imports.h>;
import devicefs.stream_writer;
import devicefs.terminal.transcoding;

export struct GuidLess {
    [[nodiscard]] auto operator()(
        const GUID &left, const GUID &right) const noexcept {
        using Words = std::array<std::uint64_t, 2>;
        const auto left_words = std::bit_cast<Words>(left);
        const auto right_words = std::bit_cast<Words>(right);
        return (std::get<0>(left_words) == std::get<0>(right_words))
            ? std::get<1>(left_words) < std::get<1>(right_words)
            : std::get<0>(left_words) < std::get<0>(right_words);
    }
};

export template <bool IncludeBraces>
constexpr auto GuidLength = IncludeBraces ? 38 : 36;

template <bool IncludeBraces>
struct FormattedGuid {
    std::array<char, GuidLength<IncludeBraces>> characters;

    static constexpr auto size() noexcept {
        return std::tuple_size_v<decltype(characters)>;
    }

    constexpr auto data() const noexcept {
        return characters.data();
    }

    [[nodiscard]] constexpr operator std::string_view() const noexcept {
        return {characters.data(), characters.size()};
    }
};

template <bool IncludeBraces>
struct std::formatter<FormattedGuid<IncludeBraces>>
    : std::formatter<std::string_view> {};

// Format a GUID with lowercase hexadecimal digits and hyphens.
// If `IncludeBraces` is true, the result is enclosed in braces.
//
// This function is intended to preserve the last Windows error value.
export template <bool IncludeBraces, class Character>
[[gsl::suppress("26447", justification:
    "Although `std::format_to_n` is not declared `noexcept` and although "
    "it can throw exceptions in some situations, there is no identified or "
    "plausible mechanism whereby it would throw an exception here, where the "
    "inputs are guaranteed to be correct, and where only integer numbers are "
    "being formatted into hexadecimal digits, and only in a locale-independent "
    "manner.")]]
auto FormatGuidToBuffer(const GUID &identifier,
    const std::span<Character, GuidLength<IncludeBraces>> buffer) noexcept {
    static constexpr auto text = std::to_array(
        "{{{:08x}-{:04x}-{:04x}-{:02x}{:02x}-"
        "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}}}");
    static constexpr auto trim = IncludeBraces ? 0 : 2;
    static constexpr auto subspan =
        std::span{text}.subspan<trim, text.size() - 2 * trim - 1>();
    static constexpr auto transcoded = [](const auto &text)
        static consteval noexcept {
        auto result = std::array<Character, text.size() + 1>{};
        std::ranges::copy(text, result.begin());
        return result;
    }(subspan);
    std::format_to_n(buffer.data(), buffer.size(), transcoded.data(),
        identifier.Data1, identifier.Data2, identifier.Data3,
        identifier.Data4[0], identifier.Data4[1], identifier.Data4[2],
        identifier.Data4[3], identifier.Data4[4], identifier.Data4[5],
        identifier.Data4[6], identifier.Data4[7]);
}

export template <bool IncludeBraces = true>
[[nodiscard]] auto FormatGuid(const GUID &guid) noexcept {
    // The `result` buffer is intentionally uninitialized because the
    // subsequent call to `std::format_to_n` overwrites every byte of the
    // buffer before anything reads the buffer.
    __pragma(warning(push))
    __pragma(warning(disable : 6001 26494))
    /* [[indeterminate]] */ FormattedGuid<IncludeBraces> result;
    FormatGuidToBuffer<IncludeBraces>(guid, std::span{result.characters});
    __pragma(warning(pop))
    return result;
}

template auto FormatGuid<true>(const GUID &);
template auto FormatGuid<false>(const GUID &);

export template <bool IncludeBraces>
class SpecialGuidSequence {
public:
    constexpr auto static kIsSpecialSequence = true;

    constexpr SpecialGuidSequence(const GUID guid) noexcept :
        guid_{guid} {}

    static constexpr auto size() noexcept {
        return GuidLength<IncludeBraces>;
    }

    auto operator()(auto *const cursor) const noexcept {
        using Character = std::remove_cvref_t<decltype(*cursor)>;
        auto *const last = cursor + size();
        FormatGuidToBuffer<IncludeBraces>(guid_,
            std::span<Character, size()>{cursor, last});
        return last;
    }
private:
    const GUID guid_;
};

// Parse a GUID with or without enclosing braces.
export [[nodiscard]] auto ParseGuid(const std::string_view value)
-> std::expected<GUID, HRESULT> {
    const auto text = value.starts_with('{') ?
        std::string{value} : std::format("{{{}}}", value);
    auto identifier = GUID{};
    const auto result = IIDFromString(
        devicefs::terminal::Transcode<wchar_t>(text).data(), &identifier);
    if (FAILED(result)) {
        return std::unexpected{result};
    }
    return identifier;
}

export [[nodiscard]] auto GenerateRandomGuid() noexcept {
    auto identifier = GUID{};
    const auto result = CoCreateGuid(&identifier);
    if (!FAILED(result)) [[likely]] {
        return identifier;
    }
    [] [[noreturn, msvc::noinline]] (const auto result) static noexcept {
        // This code is reachable only if Windows fails to load the DLLs
        // transitively required by `CoCreateGuid`. The `CoCreateGuid` function
        // has no other failure path. Such a failure indicates a defective
        // operating system state and is not a recoverable condition.
        const auto explicit_error = ExplicitHresult{result};
        devicefs::WriteToStream(devicefs::stderr,
            "backup-supervisor: terminating because Windows failed to "
            "generate a random GUID; this may suggest an operating system "
            "misconfiguration: {}\n",
            TryConstructWinError("", explicit_error));
        std::_Exit(explicit_error);
    }(result);
}
