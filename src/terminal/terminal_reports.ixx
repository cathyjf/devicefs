// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.reports;

import std;
import devicefs.terminal.safecast;

using namespace std::string_view_literals;

export namespace devicefs::terminal::detail {

enum class TerminalReport { Cursor, Size };

constexpr auto kReportPrefix = "\x1b["sv;
// Three decimal `int` values, their separators, and VT framing fit in 37 bytes.
constexpr auto kMaximumReportLength = 37uz;

[[nodiscard]] constexpr auto ReportRequest(const TerminalReport report) noexcept {
    return report == TerminalReport::Cursor ? "\x1b[6n"sv : "\x1b[18t"sv;
}

[[nodiscard]] constexpr auto ReportSuffix(const TerminalReport report) noexcept {
    return report == TerminalReport::Cursor ? "R"sv : "t"sv;
}

// Cursor and window-size queries return decimal parameters in a CSI sequence.
// Both native adapters use this parser so that a report has the same meaning
// whether the input arrives as Windows key events or Unix bytes. Recognition
// requires the entire sequence; unrelated keyboard input is left to the adapter.
// The report syntax is documented under DSR and window operations in XTerm:
// https://invisible-mirror.net/xterm/ctlseqs/ctlseqs.html
[[nodiscard]] auto ParseTerminalReport(const std::string_view text,
    const TerminalReport report) -> std::optional<std::array<int, 3>> {
    if (!text.starts_with(kReportPrefix) ||
        !text.ends_with(ReportSuffix(report)) ||
        (text.size() > kMaximumReportLength)) {
        return std::nullopt;
    }
    auto body = text.substr(kReportPrefix.size(),
        text.size() - kReportPrefix.size() - 1);
    auto values = std::array<int, 3>{};
    const auto fields = report == TerminalReport::Cursor ? 2uz : 3uz;
    for (auto index = 0uz; index < fields; ++index) {
        if (body.empty() || (body.front() < '0') || (body.front() > '9')) {
            return std::nullopt;
        }
        const auto [end, error] = std::from_chars(
            body.data(), body.data() + body.size(), values.at(index));
        if (error != std::errc{}) {
            return std::nullopt;
        }
        body.remove_prefix(FailFastCast<std::size_t>(end - body.data()));
        if ((index + 1) < fields) {
            if (!body.starts_with(';')) {
                return std::nullopt;
            }
            body.remove_prefix(1);
        }
    }
    return body.empty() ? std::optional{values} : std::nullopt;
}

}
