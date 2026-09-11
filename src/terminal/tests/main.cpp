// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

#ifdef _WIN32
    #include <devicefs/strsafe_compat.h>
#else
    // The <clocale> header unit is needed for `LC_CTYPE`.
    // It is imported on Windows and textually included on other platforms.
    #include <clocale>
#endif

import std;

#ifdef _WIN32
    import <clocale>;
#endif

import devicefs.terminal;
import devicefs.terminal.frame;
import devicefs.terminal.test_support;
import devicefs.terminal.menu;
import devicefs.terminal.menu_tests;
import devicefs.terminal.frame_tests;
import devicefs.terminal.native_tests;
import devicefs.terminal.text_tests;
import devicefs.terminal.transcoding_tests;
import devicefs.terminal.menu_measurements;
import devicefs.terminal.reports;

#ifdef _WIN32
    import devicefs.terminal.windows;
#else
    import devicefs.terminal.unix;
#endif

using namespace std::string_view_literals;
using namespace devicefs::terminal;
using namespace devicefs::terminal::tests;

namespace {

#ifdef _WIN32
using NativeConsole = WindowsConsole;
#else
using NativeConsole = UnixConsole;
#endif

auto CheckText(const std::string_view actual, const std::string_view expected)
    -> void {
    if (actual != expected) {
        throw std::runtime_error(std::format(
            "expected {:?}, received {:?}", expected, actual));
    }
}

[[nodiscard]] auto TestTextPreparation() -> bool {
    struct TextCase {
        std::string_view name;
        std::string_view input;
        std::string_view expected;
    };
    constexpr auto cases = std::array{
        TextCase{"ordinary Unicode and composed emoji"sv,
            "Debian — 日本語 — Л — e\u0301 — 👩‍💻 — ©️"sv,
            "Debian — 日本語 — Л — e\u0301 — 👩‍💻 — ©️"sv},
        TextCase{"line feeds, tabs, carriage returns, and backslashes"sv,
            "daily\nbackup\tname\r\\n"sv, "daily\\nbackup\\tname\\r\\\\n"sv},
        TextCase{"embedded NUL and other ASCII controls"sv,
            "a\0\a\b\v\f\030\032\177z"sv,
            "a\\x00\\x07\\x08\\x0B\\x0C\\x18\\x1A\\x7Fz"sv},
        // UTF-8 encodes NEL (U+0085), RI (U+008D), and ST (U+009C) below.
        // These are Next Line, Reverse Index, and String Terminator.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        TextCase{"decoded C1 controls and a stray string terminator"sv,
            "a\302\205\302\215\302\234z"sv,
            "a\\u{0085}\\u{008D}\\u{009C}z"sv},
        TextCase{"Unicode line separators and directional controls"sv,
            "\u061C\u200E\u200F\u2028\u2029\u202A\u202B\u202C"
            "\u202D\u202E\u2066\u2067\u2068\u2069"sv,
            "\\u{061C}\\u{200E}\\u{200F}\\u{2028}\\u{2029}\\u{202A}"
            "\\u{202B}\\u{202C}\\u{202D}\\u{202E}\\u{2066}\\u{2067}"
            "\\u{2068}\\u{2069}"sv},
        // DECRST 25 hides the cursor; SGR 38:2::10:20:30 selects foreground
        // RGB (10, 20, 30); SGR 0 resets attributes. All begin with CSI (`ESC [`).
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        TextCase{"CSI commands with private markers and colon parameters"sv,
            "a\033[?25l\033[38:2::10:20:30mb\033[0m"sv, "ab"sv},
        // UTF-8 C2 9B encodes CSI (U+009B); SGR 31 selects red and SGR 0 resets.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        TextCase{"decoded C1 CSI commands"sv,
            "a\xC2\x9B" "31mb\xC2\x9B" "0m"sv, "ab"sv},
        // OSC 2 (`ESC ] 2 ; title`) sets the window title. The two copies use
        // its permitted BEL and ST (`ESC \`) terminators, respectively.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        TextCase{"OSC titles terminated by BEL or ST"sv,
            "a\033]2;hidden\a\033]2;hidden\033\\b"sv, "ab"sv},
        // OSC 8 starts a hyperlink with a URI and closes it with an empty URI.
        // Both commands end with ST (`ESC \`); `label` is ordinary display text.
        // https://gist.github.com/egmontkob/eb114294efbcd5adb1944c9f3cb5feda
        TextCase{"OSC hyperlinks retaining their visible label"sv,
            "\033]8;;https://example.invalid\033\\label\033]8;;\033\\"sv,
            "label"sv},
        // `ESC P/X/^/_` introduce Device Control String, Start of String,
        // Privacy Message, and Application Program Command. ST (`ESC \`)
        // terminates each payload; the embedded BEL does not terminate DCS.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        TextCase{"DCS, SOS, PM, and APC strings"sv,
            "a\033Ppayload\a\033\\\033Xpayload\033\\"
            "\033^payload\033\\\033_payload\033\\b"sv, "ab"sv},
        // UTF-8 encodes the C1 DCS/SOS/OSC/PM/APC introducers (U+0090/0098/009D/
        // 009E/009F) and ST (U+009C). OSC uses BEL as its terminator here.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        TextCase{"decoded C1 control strings and terminators"sv,
            "a\302\220payload\302\234\302\230payload\302\234"
            "\302\235payload\a\302\236payload\302\234"
            "\302\237payload\302\234b"sv, "ab"sv},
        // DECSC (`ESC 7`) saves the cursor; DECRC (`ESC 8`) restores it;
        // `ESC ( B` designates ASCII as G0; RIS (`ESC c`) resets the terminal.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        TextCase{"other ESC commands and character-set selection"sv,
            "a\x1b" "7\x1b" "8\x1b(B\x1b" "cb"sv, "ab"sv},
        // CAN (0x18) cancels incomplete SGR 31; SUB (0x1A) cancels an unfinished
        // OSC (`ESC ]`) payload. The test resumes ordinary text after each.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        TextCase{"CAN and SUB cancelling pending commands"sv,
            "a\033[31\030b\033]hidden\032c"sv, "abc"sv},
        // SGR 31 (`CSI 31 m`, red foreground) contains intervening HT/LF/DEL
        // controls before its parameter and final byte are complete.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        TextCase{"embedded controls remaining inside discarded commands"sv,
            "a\033[3\t\n\x7f" "1mb"sv, "ab"sv},
        // An unfinished OSC payload is followed by SGR 31 (`CSI 31 m`, red).
        // Its ESC replaces the pending command in the filter's parser.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        TextCase{"a fresh ESC replacing a pending command"sv,
            "a\033]hidden\033[31mb"sv, "ab"sv},
        // OSC (`ESC ]`) starts a payload with no terminating BEL or ST.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        TextCase{"an unfinished command ending with its label"sv,
            "a\033]unterminated"sv, "a"sv},
        // CSI (`ESC [`) accepts intermediate bytes 0x20-0x2F; the following
        // non-ASCII letter deliberately violates that sequence grammar.
        // https://ecma-international.org/wp-content/uploads/ECMA-48_5th_edition_june_1991.pdf (section 5.4)
        TextCase{"a malformed command returning to ordinary text"sv,
            "a\033[ éb"sv, "aéb"sv},
        TextCase{"invalid UTF-8 bytes becoming visible notation"sv,
            "a\377\300\257\233z"sv, "a\\xFF\\xC0\\xAF\\x9Bz"sv},
        TextCase{"surrogate encodings and values beyond Unicode becoming visible bytes"sv,
            "\xed\xa0\x80 \xf4\x90\x80\x80 \xf8\x88\x80\x80\x80 \xfc\x84\x80\x80\x80\x80"sv,
            "\\xED\\xA0\\x80 \\xF4\\x90\\x80\\x80 \\xF8\\x88\\x80\\x80\\x80 \\xFC\\x84\\x80\\x80\\x80\\x80"sv},
        TextCase{"valid Unicode scalars bordering the surrogate range and maximum value"sv,
            "\ud7ff\ue000\U0010ffff"sv, "\ud7ff\ue000\U0010ffff"sv},
        TextCase{"an incomplete UTF-8 sequence at the end of a label"sv,
            "a\360\237"sv, "a\\xF0\\x9F"sv},
        // OSC (`ESC ]`) ends with BEL; DCS (`ESC P`) ends with ST (`ESC \`).
        // Their payloads deliberately contain invalid UTF-8 bytes.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        TextCase{"invalid bytes remaining inside discarded payloads"sv,
            "a\033]hidden\377\a\033P\300\257\033\\b"sv, "ab"sv},
        TextCase{"an empty label"sv, ""sv, ""sv},
    };
    auto passed = true;
    for (const auto &test_case : cases) {
        passed &= Test(test_case.name, [&test_case] {
            CheckText(PrepareTerminalText(test_case.input), test_case.expected);
        });
    }
    passed &= Test("separate labels having independent command state"sv, [] {
        // OSC (`ESC ]`) begins a payload that is deliberately left unterminated.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        CheckText(PrepareTerminalText("before\033]unfinished"sv), "before"sv);
        CheckText(PrepareTerminalText("next label"sv), "next label"sv);
    });
    return passed;
}

[[nodiscard]] auto TestLoggingFilter() -> bool {
    return Test("the copied logging filter across every byte split"sv, [] {
        constexpr auto cases = std::array{
            // SGR 31/0 selects red/resets attributes; OSC 2 sets the title and
            // ends with BEL; DCS ends with ST (`ESC \`). C2 9B is UTF-8 for CSI,
            // retained by the byte-oriented logging interface.
            // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
            std::pair{
                "A\tB\r\n\033[31mred\033[0m\033]2;hidden\aZ"
                "\033Pignored\033\\Q Л 😀 \377 \302\233"sv,
                "A\tB\nredZQ Л 😀 \377 \302\233"sv},
            // SGR 31 (`CSI 31 m`) selects red, with HT/LF inserted in the sequence.
            // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
            std::pair{"a\033[\t\n31m b"sv, "a\t\n b"sv},
            // CAN cancels an OSC payload; SUB cancels an incomplete SGR 31.
            // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
            std::pair{"a\033]hidden\030b\033[31\032c"sv, "abc"sv},
            // OSC (`ESC ]`) begins a payload with no terminator.
            // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
            std::pair{"a\033]unfinished"sv, "a"sv},
        };
        const auto copy_bytes = [](const std::string_view bytes) {
            return bytes | std::views::transform([](const char byte) {
                return std::bit_cast<char8_t>(byte);
            }) | std::ranges::to<std::u8string>();
        };
        for (const auto &test_case : cases) {
            const auto source = copy_bytes(test_case.first);
            const auto expected_bytes = copy_bytes(test_case.second);
            for (auto split = 0uz; split <= source.size(); ++split) {
                auto storage = source;
                auto filter = VtFilter{};
                auto actual = std::u8string{
                    filter.Remove(std::span{storage}.first(split))};
                actual.append(filter.Remove(std::span{storage}.subspan(split)));
                if (actual != expected_bytes) {
                    throw std::runtime_error(std::format(
                        "logging output differed when the input was split "
                        "after byte {} of {}", split, source.size()));
                }
            }
        }
    });
}

constexpr auto kDefaultCursorReplies = std::array{CursorPosition{1, 1}};

class ScriptedTerminal {
public:
    explicit ScriptedTerminal(
        const std::optional<std::span<const CursorPosition>> replies =
            std::span{kDefaultCursorReplies}) noexcept
        : replies_(replies) {}

    auto Write(const std::string_view text) -> void {
        writes.emplace_back(text);
    }

    [[nodiscard]] auto QueryCursor() -> std::optional<CursorPosition> {
        if (!replies_) {
            return std::nullopt;
        }
        Require(!replies_->empty(), "an unexpected cursor query was issued"sv);
        const auto result = replies_->front();
        *replies_ = replies_->subspan(1);
        return result;
    }

    [[nodiscard]] auto Output() const -> std::string {
        auto output = std::string{};
        for (const auto &write : writes) {
            output.append(write);
        }
        return output;
    }

    auto CheckRepliesConsumed() const -> void {
        Require(!replies_ || replies_->empty(), "an expected cursor query was not issued"sv);
    }

    std::vector<std::string> writes;

private:
    std::optional<std::span<const CursorPosition>> replies_;
};

static_assert(Terminal<ScriptedTerminal> && !FrameTerminal<ScriptedTerminal>);

[[nodiscard]] auto TestWrapping() -> bool {
    auto passed = true;
    passed &= Test("ASCII measurement preserving byte boundaries, widths, and borrowed storage in both policies"sv, [] {
        const auto printable = std::views::iota(' ', '\x7f') | std::ranges::to<std::string>();
        for (const auto &text : std::array{std::string{}, printable, std::string(1024, 'a'),
                PrepareTerminalText("\0\t\n\r\x7f\\"sv)}) {
            for (const auto &groups : std::array{
                    MeasureText<WidthPolicy::AllModes>(text),
                    MeasureText<WidthPolicy::WindowsTerminalGraphemes>(text)}) {
                Require(groups.size() == text.size(), "ASCII grouping changed the number of characters"sv);
                for (const auto &[index, group] : std::views::enumerate(groups)) {
                    Require((group.text == std::string_view{text}.substr(index, 1)) &&
                        (group.text.data() == text.data() + index) && (group.width_bound == 1),
                        std::format("ASCII group {} lost its byte, storage, or single-column width", index));
                }
            }
        }
    });
    passed &= Test("an ASCII prefix retaining its following composed character in both policies"sv, [] {
        constexpr auto text = "aéz"sv;
        constexpr auto expected = std::array{"a"sv, "é"sv, "z"sv};
        for (const auto &groups : std::array{
                MeasureText<WidthPolicy::AllModes>(text),
                MeasureText<WidthPolicy::WindowsTerminalGraphemes>(text)}) {
            Require(groups.size() == expected.size(), "mixed text lost its composed-character boundary"sv);
            for (const auto &[index, group] : std::views::enumerate(groups)) {
                CheckText(group.text, expected.at(index));
            }
        }
    });
    passed &= Test("a complete fitting label using one write and only the initial cursor query"sv,
        [] {
            auto terminal = ScriptedTerminal{};
            const auto result = WriteWrappingText(terminal, "short label"sv,
                WrappingOptions{.size = {10, 80},
                    .continuation_column = 3, .maximum_rows = 9});
            CheckText(terminal.Output(), "short label"sv);
            terminal.CheckRepliesConsumed();
            Require(terminal.writes.size() == 1, "the label was not one write"sv);
            Require(result.remaining.empty(), "part of the label was omitted"sv);
            Require(result.rows == 1, "the label did not occupy exactly one row"sv);
        });
    passed &= Test("text ending at the right margin needing only the initial cursor query"sv, [] {
        auto terminal = ScriptedTerminal{};
        const auto result = WriteWrappingText(terminal, "abcde"sv,
            WrappingOptions{.size = {10, 5},
                .continuation_column = 3, .maximum_rows = 9});
        CheckText(terminal.Output(), "abcde"sv);
        terminal.CheckRepliesConsumed();
        Require(terminal.writes.size() == 1, "the label was not one write"sv);
        Require(result.remaining.empty(), "part of the label was omitted"sv);
        Require(result.rows == 1, "filling the last column counted as a wrap"sv);
    });
    passed &= Test("one-column wrapping retaining actual row changes in cursor reports"sv, [] {
        constexpr auto replies = std::array{
            CursorPosition{1, 1}, CursorPosition{1, 1}, CursorPosition{2, 1}};
        auto terminal = ScriptedTerminal{replies};
        const auto result = WriteWrappingText(terminal, "abc"sv,
            WrappingOptions{.size = {10, 1}, .maximum_rows = 2});
        CheckText(terminal.Output(), "ab"sv);
        CheckText(result.remaining, "c"sv);
        terminal.CheckRepliesConsumed();
        Require((result.stop == WrappingStop::RowLimit) && (result.rows == 2),
            "a one-column wrap was mistaken for a pending wrap on the preceding row"sv);
    });
    passed &= Test("kitty right-margin reports placing indentation on the following row"sv, [] {
        constexpr auto replies = std::array{CursorPosition{1, 1}, CursorPosition{2, 1}};
        auto terminal = ScriptedTerminal{replies};
        auto lines = std::vector<std::string_view>{};
        const auto result = WriteWrappingText(terminal, "abcdefgh"sv,
            WrappingOptions{.size = {10, 5}, .continuation_column = 3, .maximum_rows = 9},
            [&lines](const std::string_view suffix) { lines.push_back(suffix); });
        // CUP starts the continuation at row 2, column 3.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#cursor-positioning
        CheckText(terminal.Output(), "abcde\x1b[2;3Hfgh"sv);
        terminal.CheckRepliesConsumed();
        Require((result.stop == WrappingStop::EndOfText) && (result.rows == 2) &&
            (lines == std::vector{"abcdefgh"sv, "fgh"sv}),
            "the pending wrap changed the completed row or its continuation"sv);
    });
    passed &= Test("kitty right-margin reports respecting the final permitted row"sv, [] {
        constexpr auto replies = std::array{CursorPosition{1, 1}, CursorPosition{2, 1}};
        auto terminal = ScriptedTerminal{replies};
        const auto result = WriteWrappingText(terminal, "abcdefgh"sv,
            WrappingOptions{.size = {10, 5}, .maximum_rows = 1});
        CheckText(terminal.Output(), "abcde"sv);
        CheckText(result.remaining, "fgh"sv);
        terminal.CheckRepliesConsumed();
        Require((result.stop == WrappingStop::RowLimit) && (result.rows == 1),
            "a pending wrap was mistaken for text on the next row"sv);
    });
    passed &= Test("kitty reporting a single narrow ambiguous character filling the row"sv, [] {
        constexpr auto replies = std::array{
            CursorPosition{1, 1}, CursorPosition{1, 5}, CursorPosition{2, 1}};
        auto terminal = ScriptedTerminal{replies};
        const auto result = WriteWrappingText(terminal, "abcd·x"sv,
            WrappingOptions{.size = {10, 5}, .continuation_column = 3, .maximum_rows = 9});
        // CUP starts the continuation at row 2, column 3.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#cursor-positioning
        CheckText(terminal.Output(), "abcd·\x1b[2;3Hx"sv);
        terminal.CheckRepliesConsumed();
        Require((result.stop == WrappingStop::EndOfText) && (result.rows == 2),
            "the individually observed character did not complete its original row"sv);
    });
    passed &= Test("kitty reporting a wide character wrapping and filling the next row"sv, [] {
        constexpr auto replies = std::array{CursorPosition{1, 2}, CursorPosition{3, 1}};
        auto terminal = ScriptedTerminal{replies};
        const auto result = WriteWrappingText(terminal, "日x"sv,
            WrappingOptions{.size = {10, 2}, .maximum_rows = 3});
        // CUP starts the remaining text at row 3, column 1.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#cursor-positioning
        CheckText(terminal.Output(), "日\x1b[3;1Hx"sv);
        terminal.CheckRepliesConsumed();
        Require((result.stop == WrappingStop::EndOfText) && (result.rows == 3),
            "the wide character's completed row was mistaken for a second wrap"sv);
    });
    passed &= Test("a fitting emoji sequence retaining its complete UTF-8 bytes"sv,
        [] {
            auto terminal = ScriptedTerminal{};
            const auto result = WriteWrappingText(terminal, "👩‍💻 ©️"sv,
                WrappingOptions{.size = {10, 80},
                    .continuation_column = 3, .maximum_rows = 9});
            CheckText(terminal.Output(), "👩‍💻 ©️"sv);
            terminal.CheckRepliesConsumed();
            Require(terminal.writes.size() == 1, "the label was not one write"sv);
            Require(result.remaining.empty(), "part of the label was omitted"sv);
            Require(result.rows == 1, "the label did not occupy exactly one row"sv);
        });
    passed &= Test("an empty label leaving the terminal untouched"sv, [] {
        auto terminal = ScriptedTerminal{std::span<const CursorPosition>{}};
        const auto result = WriteWrappingText(terminal, ""sv,
            WrappingOptions{.size = {10, 80},
                .continuation_column = 3, .maximum_rows = 9});
        Require(terminal.writes.empty(), "empty text caused terminal output"sv);
        Require(result.remaining.empty(), "empty text returned a remainder"sv);
        Require(result.rows == 0, "empty text occupied a row"sv);
    });
    passed &= Test("ordinary wrapping following the terminal's cursor reports"sv,
        [] {
            constexpr auto replies = std::array{
                CursorPosition{1, 1}, CursorPosition{1, 5}, CursorPosition{2, 2}};
            auto terminal = ScriptedTerminal{replies};
            const auto result = WriteWrappingText(terminal, "abcdefgh"sv,
                WrappingOptions{.size = {10, 5},
                    .continuation_column = 1, .maximum_rows = 9});
            CheckText(terminal.Output(), "abcdefgh"sv);
            terminal.CheckRepliesConsumed();
            Require(result.remaining.empty(), "part of the label was omitted"sv);
            Require(result.rows == 2, "the observed wrap did not add one row"sv);
            Require(result.stop == WrappingStop::EndOfText,
                "completed text did not report completion"sv);
        });
    passed &= Test("continuation indentation being inserted before wrapped text"sv,
        [] {
            constexpr auto replies = std::array{
                CursorPosition{1, 1}, CursorPosition{1, 5}, CursorPosition{2, 2}};
            auto terminal = ScriptedTerminal{replies};
            const auto result = WriteWrappingText(terminal, "abcdefgh"sv,
                WrappingOptions{.size = {10, 5},
                    .continuation_column = 3, .maximum_rows = 9});
            const auto output = terminal.Output();
            CheckText(PrepareTerminalText(output), "abcdefgh"sv);
            // CUP moves to row 2, column 1; ICH inserts two indentation spaces.
            // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences
            Require(output.contains("\033[2;1H\033[2@"sv),
                "two spaces were not inserted at the start of the wrapped row"sv);
            // CUP moves to row 2, column 4, after the indented character.
            // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#cursor-positioning
            Require(output.contains("\033[2;4H"sv),
                "the cursor did not advance past the indented character"sv);
            terminal.CheckRepliesConsumed();
            Require(result.remaining.empty(), "part of the label was omitted"sv);
            Require(result.rows == 2, "the indented label did not occupy two rows"sv);
        });
    passed &= Test("text continuing from the existing cursor before an indented wrap"sv,
        [] {
            constexpr auto replies = std::array{
                CursorPosition{3, 4}, CursorPosition{3, 5}, CursorPosition{4, 2}};
            auto terminal = ScriptedTerminal{replies};
            const auto result = WriteWrappingText(terminal, "abcde"sv,
                WrappingOptions{.size = {10, 5},
                    .continuation_column = 3, .maximum_rows = 8});
            // CUP moves to (row 4, column 1), ICH inserts two spaces, and CUP
            // moves to (row 4, column 4) before the remaining text is written.
            // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences
            CheckText(terminal.Output(), "abc\033[4;1H\033[2@\033[4;4Hde"sv);
            terminal.CheckRepliesConsumed();
            Require(result.remaining.empty(), "part of the label was omitted"sv);
            Require(result.rows == 2,
                "the initial partial row and continuation row were not counted"sv);
            Require(result.stop == WrappingStop::EndOfText,
                "writing from the existing cursor did not finish"sv);
        });
    passed &= Test("a pending wrap at the initial cursor receiving continuation indentation"sv,
        [] {
            constexpr auto replies = std::array{
                CursorPosition{3, 5}, CursorPosition{4, 2}};
            auto terminal = ScriptedTerminal{replies};
            const auto result = WriteWrappingText(terminal, "abc"sv,
                WrappingOptions{.size = {10, 5},
                    .continuation_column = 3, .maximum_rows = 8});
            // CUP moves to (row 4, column 1), ICH inserts two spaces, and CUP
            // moves to (row 4, column 4) before text resumes after the pending wrap.
            // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences
            CheckText(terminal.Output(), "a\033[4;1H\033[2@\033[4;4Hbc"sv);
            terminal.CheckRepliesConsumed();
            Require(result.remaining.empty(), "part of the label was omitted"sv);
            Require(result.rows == 2, "the initial pending wrap was not counted"sv);
            Require(result.stop == WrappingStop::EndOfText,
                "writing after the initial pending wrap did not finish"sv);
        });
    for (const auto maximum_rows : std::array{1, 9}) {
        passed &= Test(std::format(
            "narrow ambiguous characters filling observed space with {} available rows",
            maximum_rows), [maximum_rows] {
            constexpr auto replies = std::array{CursorPosition{1, 1}, CursorPosition{1, 3}};
            auto terminal = ScriptedTerminal{replies};
            const auto result = WriteWrappingText(terminal, "···a"sv,
                WrappingOptions{.size = {10, 5},
                    .continuation_column = 1, .maximum_rows = maximum_rows});
            CheckText(terminal.Output(), "···a"sv);
            terminal.CheckRepliesConsumed();
            Require(result.remaining.empty(), "the narrower actual text was cut off"sv);
            Require(result.rows == 1,
                "the conservative bound forced an unobserved line break"sv);
        });
    }
    passed &= Test("the final permitted row stopping before an uncertain fit"sv, [] {
        constexpr auto replies = std::array{CursorPosition{1, 1}, CursorPosition{1, 5}};
        auto terminal = ScriptedTerminal{replies};
        const auto result = WriteWrappingText(terminal, "abcd·"sv,
            WrappingOptions{.size = {1, 5},
                .continuation_column = 1, .maximum_rows = 1});
        CheckText(terminal.Output(), "abcd"sv);
        CheckText(result.remaining, "·"sv);
        terminal.CheckRepliesConsumed();
        Require(result.rows == 1, "the row limit was exceeded"sv);
        Require(result.stop == WrappingStop::RowLimit,
            "an uncertain final-row fit did not report the row limit"sv);
    });
    passed &= Test("an oversized joined sequence returning untouched text"sv, [] {
        constexpr auto text = "e\u0301\u0301\u0301\u0301"sv;
        auto terminal = ScriptedTerminal{};
        const auto result = WriteWrappingText(terminal, text,
            WrappingOptions{.size = {10, 3},
                .continuation_column = 1, .maximum_rows = 9});
        terminal.CheckRepliesConsumed();
        Require(terminal.writes.empty(), "an oversized sequence was partly written"sv);
        CheckText(result.remaining, text);
        Require(result.rows == 0, "unwritten text occupied a row"sv);
        Require(result.stop == WrappingStop::OversizedCluster,
            "an oversized sequence did not report the width limitation"sv);
    });
    for (const auto &test_case : std::array{
            std::pair{"a letter with eight combining marks"sv,
                "e\u0301\u0301\u0301\u0301\u0301\u0301\u0301\u0301xy"sv},
            std::pair{"a family emoji with joined characters"sv, "👩‍👩‍👧‍👦!"sv}}) {
        passed &= Test(std::format(
            "the grapheme policy fitting {} intact in three columns", test_case.first),
            [text = test_case.second] {
                auto terminal = ScriptedTerminal{};
                const auto result = WriteWrappingText<WidthPolicy::WindowsTerminalGraphemes>(
                    terminal, text,
                    WrappingOptions{.size = {1, 3},
                        .continuation_column = 1, .maximum_rows = 1});
                CheckText(terminal.Output(), text);
                terminal.CheckRepliesConsumed();
                Require(terminal.writes.size() == 1,
                    "the fitting composition was not written in one batch"sv);
                Require(result.remaining.empty(), "part of the label was omitted"sv);
                Require(result.rows == 1, "the label did not occupy exactly one row"sv);
                Require(result.stop == WrappingStop::EndOfText,
                    "the fitting composition was treated as oversized"sv);
            });
    }
    passed &= Test("the grapheme policy wrapping and indenting a complete family emoji"sv,
        [] {
            constexpr auto replies = std::array{
                CursorPosition{1, 1}, CursorPosition{1, 5}, CursorPosition{2, 3}};
            auto terminal = ScriptedTerminal{replies};
            constexpr auto text = "abcd👩‍👩‍👧‍👦x"sv;
            const auto result = WriteWrappingText<WidthPolicy::WindowsTerminalGraphemes>(
                terminal, text,
                WrappingOptions{.size = {10, 5},
                    .continuation_column = 3, .maximum_rows = 9});
            const auto output = terminal.Output();
            CheckText(PrepareTerminalText(output), text);
            Require(std::ranges::count(terminal.writes, "👩‍👩‍👧‍👦"sv) == 1,
                "the family emoji was not submitted intact at the right margin"sv);
            // CUP moves to (row 2, column 1), ICH inserts two spaces, and CUP
            // moves to (row 2, column 5), after the two-column emoji.
            // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences
            Require(output.contains("\033[2;1H\033[2@\033[2;5H"sv),
                "two spaces were not inserted before the wrapped two-column emoji"sv);
            terminal.CheckRepliesConsumed();
            Require(result.remaining.empty(), "part of the label was omitted"sv);
            Require(result.rows == 2, "the indented label did not occupy two rows"sv);
            Require(result.stop == WrappingStop::EndOfText,
                "the wrapped composition did not finish"sv);
        });
    passed &= Test("the grapheme policy reserving two columns for ambiguous characters"sv,
        [] {
            constexpr auto replies = std::array{CursorPosition{1, 1}, CursorPosition{1, 2}};
            auto terminal = ScriptedTerminal{replies};
            const auto result = WriteWrappingText<WidthPolicy::WindowsTerminalGraphemes>(
                terminal, "·x"sv,
                WrappingOptions{.size = {1, 2},
                    .continuation_column = 1, .maximum_rows = 1});
            CheckText(terminal.Output(), "·"sv);
            CheckText(result.remaining, "x"sv);
            terminal.CheckRepliesConsumed();
            Require(result.stop == WrappingStop::RowLimit,
                "text after a possibly wide character overran the last row"sv);
        });
    passed &= Test("zero-width prefixes staying with the following visible group"sv,
        [] {
            constexpr auto replies = std::array{CursorPosition{1, 1}};
            auto terminal = ScriptedTerminal{replies};
            const auto result = WriteWrappingText<WidthPolicy::WindowsTerminalGraphemes>(
                terminal, "\u200B·"sv,
                WrappingOptions{.size = {1, 2},
                    .continuation_column = 1, .maximum_rows = 1});
            CheckText(terminal.Output(), "\u200B·"sv);
            CheckText(result.remaining, ""sv);
            terminal.CheckRepliesConsumed();
            Require(result.stop == WrappingStop::EndOfText,
                "the zero-width prefix was separated from its fitting visible group"sv);
        });
    passed &= Test("an emoji variation selector remaining with its base in one write"sv,
        [] {
            constexpr auto replies = std::array{
                CursorPosition{1, 1}, CursorPosition{1, 5}, CursorPosition{2, 3}};
            auto terminal = ScriptedTerminal{replies};
            const auto result = WriteWrappingText(terminal, "abcd©️x"sv,
                WrappingOptions{.size = {10, 5},
                    .continuation_column = 1, .maximum_rows = 9});
            CheckText(terminal.Output(), "abcd©️x"sv);
            Require(std::ranges::count(terminal.writes, "©️"sv) == 1,
                "the base and its variation selector were not written together"sv);
            terminal.CheckRepliesConsumed();
            Require(result.remaining.empty(), "part of the label was omitted"sv);
            Require(result.rows == 2, "the emoji's observed wrap was lost"sv);
        });
    passed &= Test("indentation filling a row before text continues on the next row"sv,
        [] {
            constexpr auto replies = std::array{
                CursorPosition{1, 1}, CursorPosition{1, 5}, CursorPosition{2, 2}};
            auto terminal = ScriptedTerminal{replies};
            const auto result = WriteWrappingText(terminal, "abcdefg"sv,
                WrappingOptions{.size = {10, 5},
                    .continuation_column = 5, .maximum_rows = 9});
            const auto output = terminal.Output();
            CheckText(PrepareTerminalText(output), "abcdefg"sv);
            // CUP moves to row 3, column 5 for the next indented continuation.
            // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#cursor-positioning
            Require(output.contains("\033[3;5H"sv),
                "continuation text did not move past the row filled by indentation"sv);
            terminal.CheckRepliesConsumed();
            Require(result.remaining.empty(), "part of the label was omitted"sv);
            Require(result.rows == 3, "the explicit continuation row was not counted"sv);
        });
    for (const auto &test_case : std::array{
            std::pair{"a column beyond the supplied width"sv, CursorPosition{3, 6}},
            std::pair{"an unusable column"sv, CursorPosition{3, 0}},
            std::pair{"text moving above the previous row"sv, CursorPosition{2, 2}},
            std::pair{"text moving down more than one row"sv, CursorPosition{5, 2}},
            std::pair{"a previously fitting batch wrapping"sv, CursorPosition{4, 2}}}) {
        passed &= Test(std::format(
            "{} requesting redraw without further output", test_case.first),
            [reply = test_case.second] {
                const auto replies = std::array{CursorPosition{3, 1}, reply};
                auto terminal = ScriptedTerminal{replies};
                const auto result = WriteWrappingText(terminal, "abcdefgh"sv,
                    WrappingOptions{.size = {10, 5},
                        .continuation_column = 3, .maximum_rows = 8});
                CheckText(terminal.Output(), "abcde"sv);
                CheckText(result.remaining, "fgh"sv);
                terminal.CheckRepliesConsumed();
                Require(result.stop == WrappingStop::RedrawRequired,
                    "the changed layout did not request a redraw"sv);
            });
    }
    for (const auto reply : std::array{CursorPosition{2, 2}, CursorPosition{4, 5}}) {
        passed &= Test(std::format(
            "an unusable wrap report at row {}, column {} stopping before indentation",
            reply.row, reply.column), [reply] {
            const auto replies = std::array{CursorPosition{3, 1}, CursorPosition{3, 5}, reply};
            auto terminal = ScriptedTerminal{replies};
            const auto result = WriteWrappingText(terminal, "abcdef"sv,
                WrappingOptions{.size = {10, 5},
                    .continuation_column = 3, .maximum_rows = 8});
            CheckText(terminal.Output(), "abcdef"sv);
            Require(result.remaining.empty(), "written text was returned as unwritten"sv);
            terminal.CheckRepliesConsumed();
            Require(result.stop == WrappingStop::RedrawRequired,
                "a disrupted final write was reported as successful layout"sv);
        });
    }
    passed &= Test("an unavailable initial cursor report leaving the display untouched"sv, [] {
        auto terminal = ScriptedTerminal{std::nullopt};
        const auto result = WriteWrappingText(terminal, "abcdefgh"sv,
            WrappingOptions{.size = {10, 5},
                .continuation_column = 3, .maximum_rows = 9});
        Require(terminal.writes.empty(), "text was written without an initial cursor report"sv);
        CheckText(result.remaining, "abcdefgh"sv);
        Require(result.rows == 0, "unwritten text occupied a row"sv);
        Require(result.stop == WrappingStop::RedrawRequired,
            "a missing cursor report did not request a redraw"sv);
    });
    for (const auto &test_case : std::array{
            std::pair{"zero allowed rows"sv, WrappingOptions{
                .size = {1, 2}, .continuation_column = 3, .maximum_rows = 0}},
            std::pair{"zero screen rows"sv, WrappingOptions{.size = {0, 8}}},
            std::pair{"negative screen rows"sv, WrappingOptions{.size = {-1, 8}}},
            std::pair{"zero screen columns"sv, WrappingOptions{.size = {3, 0}}},
            std::pair{"negative screen columns"sv, WrappingOptions{.size = {3, -1}}}}) {
        passed &= Test(std::format("{} returning retained text without terminal access", test_case.first),
            [options = test_case.second] {
                for (const auto text : std::array{""sv, "abcdefgh"sv}) {
                    auto terminal = ScriptedTerminal{std::span<const CursorPosition>{}};
                    const auto result = WriteWrappingText(terminal, text, options);
                    Require(terminal.writes.empty(), "an empty area caused output"sv);
                    CheckText(result.remaining, text);
                    Require(result.rows == 0, "unwritten text occupied a row"sv);
                    Require(result.stop == (text.empty() ? WrappingStop::EndOfText : WrappingStop::RowLimit),
                        "an empty area did not distinguish completed empty text from retained input"sv);
                }
            });
    }
    passed &= Test("invalid wrapping capacities being rejected before cursor observation"sv, [] {
        for (const auto &options : std::array{
                WrappingOptions{.size = {3, 8}, .maximum_rows = -1},
                WrappingOptions{.size = {3, 8}, .continuation_column = 0},
                WrappingOptions{.size = {3, 8}, .continuation_column = -1},
                WrappingOptions{.size = {3, 8}, .trailing_columns = -1}}) {
            auto terminal = ScriptedTerminal{std::span<const CursorPosition>{}};
            auto rejected = false;
            try {
                std::ignore = WriteWrappingText(terminal, "text"sv, options);
            } catch (const std::invalid_argument &) {
                rejected = true;
            }
            Require(rejected, std::format(
                "wrapping accepted maximum_rows {}, continuation_column {}, trailing_columns {}",
                options.maximum_rows, options.continuation_column, options.trailing_columns));
            Require(terminal.writes.empty(), "invalid wrapping options caused output"sv);
        }
    });
    passed &= Test("redrawing the original text after obtaining a narrower window"sv, [] {
        constexpr auto text = "abcdefgh"sv;
        constexpr auto first_reply = std::array{CursorPosition{1, 1}, CursorPosition{2, 2}};
        auto interrupted = ScriptedTerminal{first_reply};
        const auto first = WriteWrappingText(interrupted, text,
            WrappingOptions{.size = {10, 5},
                .continuation_column = 1, .maximum_rows = 9});
        Require(first.stop == WrappingStop::RedrawRequired,
            "the initial layout did not request a redraw"sv);
        constexpr auto replies = std::array{
            CursorPosition{1, 1}, CursorPosition{1, 4}, CursorPosition{2, 2}};
        auto redrawn = ScriptedTerminal{replies};
        const auto result = WriteWrappingText(redrawn, text,
            WrappingOptions{.size = {10, 4},
                .continuation_column = 1, .maximum_rows = 9});
        CheckText(redrawn.Output(), text);
        Require(result.remaining.empty(), "redrawing omitted part of the original text"sv);
        Require(result.stop == WrappingStop::EndOfText, "the redraw did not finish"sv);
        redrawn.CheckRepliesConsumed();
    });
    passed &= Test("the remaining screen rows limiting an oversized row request"sv, [] {
        constexpr auto replies = std::array{
            CursorPosition{9, 1}, CursorPosition{9, 5},
            CursorPosition{10, 2}, CursorPosition{10, 5}};
        auto terminal = ScriptedTerminal{replies};
        const auto result = WriteWrappingText(terminal, "abcdefghijk"sv,
            WrappingOptions{.size = {10, 5},
                .continuation_column = 1, .maximum_rows = 20});
        CheckText(terminal.Output(), "abcdefghij"sv);
        CheckText(result.remaining, "k"sv);
        terminal.CheckRepliesConsumed();
        Require(result.rows == 2, "the bottom two screen rows were not counted"sv);
        Require(result.stop == WrappingStop::RowLimit,
            "the requested row count allowed text to scroll past the screen"sv);
    });
    return passed;
}

constexpr auto EXIT_SUCCESS = 0;
constexpr auto EXIT_FAILURE = 1;

[[nodiscard]] auto TestReports() -> bool {
    struct ReportCase {
        std::string_view name;
        std::u32string_view input;
        detail::TerminalReport report;
        std::optional<std::array<int, 3>> expected;
        std::u32string_view retained;
    };
    const auto cases = std::array{
        // CPR (`CSI row;column R`) reports row 12, column 34 between two keys.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
        ReportCase{"cursor reply surrounded by keyboard input"sv,
            U"a\x1b[12;34Rb"sv, detail::TerminalReport::Cursor,
            std::array{12, 34, 0}, U"ab"sv},
        // `CSI B` is the Down key in normal cursor-key mode. XTWINOPS replies
        // with `CSI 8;rows;columns t`, here describing 24 rows and 80 columns.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        ReportCase{"size reply following an arrow key"sv,
            U"\x1b[B\x1b[8;24;80t"sv, detail::TerminalReport::Size,
            std::array{8, 24, 80}, U"\x1b[B"sv},
        // An incomplete CPR is followed by `CSI 3;4 R`, a complete cursor report.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
        ReportCase{"a new escape replacing an incomplete reply"sv,
            U"\x1b[12;\x1b[3;4R"sv, detail::TerminalReport::Cursor,
            std::array{3, 4, 0}, U"\x1b[12;"sv},
        // CPR (`CSI row;column R`) is deliberately missing its final `R`.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
        ReportCase{"an incomplete reply remaining available as input"sv,
            U"\x1b[12;34"sv, detail::TerminalReport::Cursor,
            std::nullopt, U"\x1b[12;34"sv},
        // CPR requires decimal coordinates. The first reply instead contains
        // a non-ASCII character; the following `CSI 5;6 R` is valid.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
        ReportCase{"non-ASCII input invalidating a candidate reply"sv,
            U"\x1b[12;\u1234R\x1b[5;6R"sv, detail::TerminalReport::Cursor,
            std::array{5, 6, 0}, U"\x1b[12;\u1234R"sv},
        // CPR (`CSI row;column R`) first supplies a row outside `int` range,
        // then a valid report for row 5, column 6.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
        ReportCase{"an overflowing report preceding a valid reply"sv,
            U"\x1b[2147483648;1R\x1b[5;6R"sv, detail::TerminalReport::Cursor,
            std::array{5, 6, 0}, U"\x1b[2147483648;1R"sv},
        // XTWINOPS reports a 24-by-80 text area (`CSI 8;24;80 t`) before the
        // requested CPR (`CSI 5;6 R`). Only the cursor report belongs to the query.
        // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
        ReportCase{"a report for another query remaining available as input"sv,
            U"\x1b[8;24;80t\x1b[5;6R"sv, detail::TerminalReport::Cursor,
            std::array{5, 6, 0}, U"\x1b[8;24;80t"sv},
        // The first CPR (`CSI row;column R`) exceeds the parser's length limit
        // with leading zeros; the following report supplies row 5, column 6.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
        ReportCase{"an overlong candidate preceding a valid reply"sv,
            U"\x1b[00000000000000000000000000000000000001;1R\x1b[5;6R"sv,
            detail::TerminalReport::Cursor, std::array{5, 6, 0},
            U"\x1b[00000000000000000000000000000000000001;1R"sv},
    };
    auto passed = true;
    for (const auto &test : cases) {
        passed &= Test(test.name, [&test] {
            auto reader = detail::ReportReader<std::size_t>{test.report};
            auto retained = std::u32string{test.input};
            const auto reply = [&test, &reader, &retained] {
                for (auto position = 0uz; position < test.input.size(); ++position) {
                    if (const auto values = reader.Push(test.input[position], position)) {
                        for (const auto removed : reader.Positions() | std::views::reverse) {
                            retained.erase(removed, 1);
                        }
                        return values;
                    }
                }
                return std::optional<std::array<int, 3>>{};
            }();
            Require(reply == test.expected, "the recognized report had unexpected fields"sv);
            Require(retained == test.retained, "report recognition consumed other input"sv);
        });
    }
    return passed;
}

[[nodiscard]] auto SelfTest() -> int {
    auto passed = TestTextPreparation();
    passed &= TestLoggingFilter();
    passed &= TestGeneratedText();
    passed &= TestAsciiPredicate();
    passed &= TestTranscoding();
    passed &= TestWrapping();
    passed &= TestMenu();
    passed &= RunFrameTests();
    passed &= TestReports();
    std::println("\nTerminal library self-tests {}.", passed ? "passed" : "failed");
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

constexpr auto kHelp = R"(Usage:
  devicefs-terminal-test --self-test
  devicefs-terminal-test --native-test
  devicefs-terminal-test --menu
  devicefs-terminal-test --measure-menu
  devicefs-terminal-test --text TEXT
  devicefs-terminal-test --file FILENAME

With no arguments, display sample text in the attached console.
The text is filtered, wrapped, and indented by two columns after each wrap.
Text begins at the current cursor position and uses the remaining screen rows.
--menu opens submenus; Escape returns to the parent or closes the main menu.
--measure-menu reports each menu update's time and terminal traffic after exit.
)"sv;

template <MenuTerminal Console = NativeConsole>
[[nodiscard]] auto MenuDemo() -> int {
    constexpr auto title = "DeviceFs terminal menu demonstration"sv;
    const auto draw_header = [title](auto &frame) {
        frame.Write("{}", title);
        frame.Write("\nChoose an entry to open a submenu.\n\n"sv);
    };
    auto entries = std::vector<std::string>{
        "Installation", "Schedule backups", "Browse backups", "Open backup console",
        "Accented names: café and naïve", "日本語 — é — 👩‍💻 — ©️",
        // SGR 31 (`CSI 31 m`) requests red foreground; label preparation removes it.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#text-formatting
        "Embedded controls: daily\nbackup\tname\033[31m (displayed as text)",
        [] {
            auto name = std::string{};
            for (auto section = 1; section <= 32; ++section) {
                name.append(std::format(
                    "[{:02}] abcdefghijklmnopqrstuvwxyz 0123456789; ", section));
            }
            return name + "— end of the complete name";
        }(),
    };
    for (auto index = entries.size(); index < 40; ++index) {
        entries.push_back(std::format("Example backup {:02}", index + 1));
    }
    auto labels = std::vector<std::string_view>{};
    labels.reserve(entries.size());
    for (const auto &entry : entries) {
        labels.push_back(entry);
    }
    struct MenuResult {
        std::optional<std::size_t> selection;
        std::vector<MenuMeasurement> measurements;
    };
    const auto [selection, measurements] = [&] {
        auto terminal = Console{};
        const auto selection = [&terminal, &draw_header, &labels, title]() -> std::optional<std::size_t> {
            const auto screen = terminal.EnterScreen();
            auto initial_selection = 0uz;
            while (const auto entry = SelectMenuItem(terminal, draw_header, labels, {}, initial_selection)) {
                initial_selection = *entry;
                const auto draw_submenu_header = [title,
                    label = labels.at(*entry)](auto &frame) {
                    frame.Write("{}", title);
                    frame.Write("\nChoose an entry to print its index.\n"sv);
                    frame.WriteLine("{}", label);
                    frame.Write("\n"sv);
                };
                constexpr auto submenu_entries = std::array{
                    "First submenu option"sv, "Second submenu option"sv, "Third submenu option"sv};
                if (const auto submenu_entry = SelectMenuItem(terminal, draw_submenu_header, submenu_entries)) {
                    return submenu_entry;
                }
            }
            return std::nullopt;
        }();
        if constexpr (std::same_as<Console, MeasuringConsole<NativeConsole>>) {
            return MenuResult{.selection = selection, .measurements = std::move(terminal.measurements)};
        } else {
            return MenuResult{.selection = selection, .measurements = {}};
        }
    }();
    if (selection) {
        std::println("Selected submenu index: {}", *selection);
    } else {
        std::println("Menu cancelled.");
    }
    PrintMenuMeasurements(measurements);
    return EXIT_SUCCESS;
}

}

auto main(const int argc, char *const argv[]) -> int {
#ifdef _WIN32
    constexpr auto kLocaleCodeset = ".UTF8";
#else
    constexpr auto kLocaleCodeset = "C.UTF-8";
#endif
    const auto arguments = std::span{argv, argv + argc};
    try {
        if (std::setlocale(LC_CTYPE, kLocaleCodeset) == nullptr) {
            throw std::runtime_error(std::format(
                "failed to set the character encoding to {}", kLocaleCodeset));
        }
        if ((arguments.size() == 2) &&
            (std::string_view{arguments[1]} == "--menu"sv)) {
            return MenuDemo();
        }
        if ((arguments.size() == 2) &&
            (std::string_view{arguments[1]} == "--measure-menu"sv)) {
            return MenuDemo<MeasuringConsole<NativeConsole>>();
        }
        if ((arguments.size() == 2) &&
            (std::string_view{arguments[1]} == "--self-test"sv)) {
            return SelfTest();
        }
        if ((arguments.size() == 2) &&
            (std::string_view{arguments[1]} == "--native-test"sv)) {
            return RunNativeTests();
        }
        if ((arguments.size() == 2) &&
            (std::string_view{arguments[1]} == "--help"sv)) {
            std::print("{}", kHelp);
            return EXIT_SUCCESS;
        }
        const auto text = [arguments] {
            if (arguments.size() == 1) {
                return std::string{
                    "DeviceFs terminal text: accented names café and naïve; "
                    "日本語; composed characters e\u0301; emoji 👩‍💻 and ©️. "
                    "This sentence continues so the terminal can choose where "
                    "the text wraps and the library can indent each continuation. "
                    "An embedded newline is displayed explicitly:\nnext line."};
            }
            if (arguments.size() == 3) {
                if (std::string_view{arguments[1]} == "--text"sv) {
                    return std::string{arguments[2]};
                }
                if (std::string_view{arguments[1]} == "--file"sv) {
                    auto input = std::ifstream{arguments[2], std::ios::binary};
                    auto contents = std::string{
                        std::istreambuf_iterator<char>{input},
                        std::istreambuf_iterator<char>{}};
                    if (!input) {
                        throw std::runtime_error(std::format(
                            "could not read input text file '{}'", arguments[2]));
                    }
                    return contents;
                }
            }
            throw std::invalid_argument(std::string{kHelp});
        }();
        const auto prepared = PrepareTerminalText(text);
        const auto result = [&prepared]() -> std::optional<WrappingResult> {
            auto console = NativeConsole{};
            const auto size = console.QuerySize();
            if (!size) {
                console.Write(prepared);
                return std::nullopt;
            }
            return WriteWrappingText(console, prepared,
                WrappingOptions{.size = *size,
                    .continuation_column = std::min(3, size->columns),
                    .maximum_rows = size->rows});
        }();
        std::println();
        if (!result) {
            std::println("The terminal did not report its dimensions; "
                "the text was displayed without continuation indentation.");
        } else if (result->stop == WrappingStop::RedrawRequired) {
            std::println("The terminal layout could not be confirmed; "
                "the displayed text may be incomplete.");
        } else if (!result->remaining.empty()) {
            std::println("{} {} bytes remain.",
                result->stop == WrappingStop::OversizedCluster
                    ? "The next joined sequence exceeds the conservative width limit."
                    : "The available display rows are full.",
                result->remaining.size());
        }
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::println(std::cerr, "devicefs-terminal-test: {}", error.what());
        return EXIT_FAILURE;
    }
}
