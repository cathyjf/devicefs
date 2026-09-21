// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.guid_formatter;

import std;
import <devicefs/windows_imports.h>;
import devicefs.terminal.transcoding;

// Format a GUID with lowercase hexadecimal digits and hyphens.
// If `IncludeBraces` is true, the result is enclosed in braces.
export template <bool IncludeBraces = true>
[[nodiscard]] auto FormatGuid(const GUID &identifier) {
    return std::format(
        "{}{:08x}-{:04x}-{:04x}-{:02x}{:02x}-"
        "{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{}",
        IncludeBraces ? "{" : "",
        identifier.Data1, identifier.Data2, identifier.Data3,
        identifier.Data4[0], identifier.Data4[1], identifier.Data4[2],
        identifier.Data4[3], identifier.Data4[4], identifier.Data4[5],
        identifier.Data4[6], identifier.Data4[7],
        IncludeBraces ? "}" : "");
}

// Parse a GUID with or without enclosing braces.
export [[nodiscard]] auto ParseGuid(const std::string_view value)
-> std::expected<GUID, HRESULT> {
    const auto text = value.starts_with('{') ?
        std::string{value} : std::format("{{{}}}", value);
    auto identifier = GUID{};
    const auto error = IIDFromString(
        devicefs::terminal::Transcode<wchar_t>(text).data(), &identifier);
    if (FAILED(error)) {
        return std::unexpected{error};
    }
    return identifier;
}
