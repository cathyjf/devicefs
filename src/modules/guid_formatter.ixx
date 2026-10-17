// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.guid_formatter;

import std;
import <devicefs/windows_imports.h>;
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

template <bool IncludeBraces>
struct FormattedGuid {
    std::array<char, IncludeBraces ? 38 : 36> characters;

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
export template <bool IncludeBraces = true>
[[nodiscard]] auto FormatGuid(const GUID &identifier) {
    // The `result` buffer is intentionally uninitialized because the
    // subsequent call to `std::format_to_n` overwrites every byte of the
    // buffer before anything reads the buffer.
    //
    // This suppression uses `#pragma warning` rather than the usual
    // `[[gsl::suppress]]` annotation because using `[[gsl::suppress]]` to
    // annotate this function causes MSVC to crash.
#pragma warning(push)
#pragma warning(disable : 6001 26494)
    /* [[indeterminate]] */ FormattedGuid<IncludeBraces> result;
    std::ignore = std::format_to_n(result.characters.begin(),
        std::tuple_size_v<decltype(result.characters)>,
        "{}{:08x}-{:04x}-{:04x}-{:02x}{:02x}-"
        "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{}",
        IncludeBraces ? "{" : "",
        identifier.Data1, identifier.Data2, identifier.Data3,
        identifier.Data4[0], identifier.Data4[1], identifier.Data4[2],
        identifier.Data4[3], identifier.Data4[4], identifier.Data4[5],
        identifier.Data4[6], identifier.Data4[7],
        IncludeBraces ? "}" : "");
#pragma warning(pop)
    return result;
}

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
