// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.reports;

import std;
import devicefs.terminal.safecast;
import devicefs.terminal.vt;

using namespace std::string_view_literals;

export namespace devicefs::terminal::detail {

enum class TerminalReport { Cursor, Size, KeyboardFlags, ModifiedKeys };
enum class KeyboardProtocol { Legacy, Kitty, Xterm };

// ESC followed by `[` is the seven-bit Control Sequence Introducer (CSI),
// which begins both cursor-position and window-size reports.
// https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#cursor-positioning
constexpr auto kReportPrefix = vt::kCsiPrefix;
// Three decimal `int` values, their separators, and VT framing fit in 37 bytes.
constexpr auto kMaximumReportLength = 37uz;

[[nodiscard]] constexpr auto ReportRequest(const TerminalReport report) noexcept {
    switch (report) {
    case TerminalReport::Cursor: return vt::kRequestCursorPosition;
    case TerminalReport::Size: return vt::kRequestTextAreaSize;
    case TerminalReport::KeyboardFlags: return vt::kRequestKeyboardFlags;
    case TerminalReport::ModifiedKeys: return vt::kRequestModifiedKeys;
    }
    std::unreachable();
}

[[nodiscard]] constexpr auto ReportPrefix(const TerminalReport report) noexcept {
    switch (report) {
    case TerminalReport::KeyboardFlags: return vt::kKeyboardFlagsReplyPrefix;
    case TerminalReport::ModifiedKeys: return vt::kModifiedKeysReplyPrefix;
    default: return kReportPrefix;
    }
}

[[nodiscard]] constexpr auto ReportSuffix(const TerminalReport report) noexcept {
    // The final character identifies each report within its CSI family.
    // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
    switch (report) {
    case TerminalReport::Cursor: return "R"sv;
    case TerminalReport::Size: return "t"sv;
    case TerminalReport::KeyboardFlags: return "u"sv;
    case TerminalReport::ModifiedKeys: return "m"sv;
    }
    std::unreachable();
}

// Terminal queries return decimal parameters in a CSI sequence.
// Both native adapters use this parser so that a report has the same meaning
// whether the input arrives as Windows key events or Unix bytes. Recognition
// requires the entire sequence; unrelated keyboard input is left to the adapter.
// The report syntax is documented under DSR and window operations in XTerm:
// https://invisible-mirror.net/xterm/ctlseqs/ctlseqs.html
[[nodiscard]] auto ParseTerminalReport(const std::string_view text,
    const TerminalReport report) -> std::optional<std::array<int, 3>> {
    const auto prefix = ReportPrefix(report);
    if (!text.starts_with(prefix) ||
        !text.ends_with(ReportSuffix(report)) ||
        (text.size() > kMaximumReportLength)) {
        return std::nullopt;
    }
    auto body = text.substr(prefix.size(), text.size() - prefix.size() - 1);
    auto values = std::array<int, 3>{};
    const auto fields = (report == TerminalReport::Cursor) ? 2uz :
        ((report == TerminalReport::Size) ? 3uz : 1uz);
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

// `ReportReader` recognizes one requested terminal reply as input arrives. An ESC
// starts a new candidate; invalid characters discard that candidate, and a
// complete reply supplies its numeric fields. Tracking each character's input
// position lets the caller remove precisely the recognized report from its
// queue. Windows uses event iterators because other events can occur between
// reply characters; Unix uses byte offsets into its input buffer.
template <typename Position>
class ReportReader {
public:
    explicit ReportReader(const TerminalReport report) : report_{report} {
        text_.reserve(kMaximumReportLength);
        positions_.reserve(kMaximumReportLength);
    }

    [[nodiscard]] auto Push(const char32_t character, const Position position)
        -> std::optional<std::array<int, 3>> {
        // ESC (0x1B) starts a new candidate for the seven-bit CSI report prefix.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#cursor-positioning
        if (character == vt::kEscape) {
            text_.assign(1, vt::kEscape);
            positions_.clear();
            positions_.push_back(position);
            return std::nullopt;
        }
        if (text_.empty()) {
            return std::nullopt;
        }
        // The report syntax cited above consists of ASCII characters. Values
        // above 0x7f therefore cannot be part of a report.
        if ((character > 0x7f) || (text_.size() == kMaximumReportLength)) {
            text_.clear();
            return std::nullopt;
        }
        // char32_t is unsigned, and the check above excludes values above 127.
        // Every remaining value fits in char, so the conversion preserves it.
        text_.push_back(FailFastCast<char>(character));
        positions_.push_back(position);
        const auto prefix = ReportPrefix(report_);
        if (text_.size() <= prefix.size()) {
            if (!prefix.starts_with(text_)) {
                text_.clear();
            }
            return std::nullopt;
        }
        if (text_.ends_with(ReportSuffix(report_))) {
            const auto values = ParseTerminalReport(text_, report_);
            text_.clear();
            return values;
        }
        if (!((character >= U'0') && (character <= U'9')) && (character != U';')) {
            text_.clear();
        }
        return std::nullopt;
    }

    [[nodiscard]] auto Positions() const -> std::span<const Position> {
        return positions_;
    }

    [[nodiscard]] auto Report() const noexcept {
        return report_;
    }

private:
    TerminalReport report_;
    std::string text_;
    std::vector<Position> positions_;
};

}
