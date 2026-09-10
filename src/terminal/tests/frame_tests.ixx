// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.frame_tests;

import std;
import devicefs.terminal;
import devicefs.terminal.test_support;
import devicefs.terminal.frame;
import devicefs.terminal.drawing;
import devicefs.terminal.formatting;

using namespace std::string_view_literals;
using namespace devicefs::terminal;
using namespace devicefs::terminal::tests;

namespace {

// This terminal has explicit widths for the Unicode text used by these tests.
// MeasureText identifies complete groups, but the fake never uses their width
// estimates. Each occupied cell records its entire glyph, allowing an overwrite
// of either half of a wide glyph to remove the old glyph's other half as well.
// Output records distinguish text repainted by Flip from unchanged screen cells.
class FrameConsole {
public:
    struct Cell {
        std::string text;
        int first_column;
        int width;
        bool reverse;
    };

    explicit FrameConsole(const TerminalSize size) noexcept : size_{size} {}

    auto Write(std::string_view text) -> void {
        ++writes;
        bytes += text.size();
        while (!text.empty()) {
            if (text.starts_with("\x1b["sv)) {
                text.remove_prefix(2);
                const auto end = text.find_first_of("HJKXmhl"sv);
                Require(end != std::string_view::npos, "unterminated frame command"sv);
                const auto parameters = text.substr(0, end);
                const auto command = text.at(end);
                text.remove_prefix(end + 1);
                const auto number = parameters.empty() ? 0 :
                    std::stoi(std::string{parameters});
                if (command == 'H') {
                    ++cursor_moves;
                    const auto separator = parameters.find(';');
                    cursor_ = {.row = number == 0 ? 1 : number,
                        .column = separator == std::string_view::npos ? 1 :
                            std::stoi(std::string{parameters.substr(separator + 1)})};
                    pending_wrap_ = false;
                } else if (command == 'm') {
                    ++attribute_changes;
                    Require((number == 0) || (number == 7), "unexpected frame attribute"sv);
                    reverse_ = number == 7;
                } else if (command == 'J') {
                    Require(number == 2, "unexpected screen erasure"sv);
                    cells_.clear();
                    ++screen_erases;
                } else if ((command == 'X') || (command == 'K')) {
                    const auto first = ((command == 'K') && (number == 2)) ?
                        1 : cursor_.column;
                    const auto count = command == 'X' ? std::max(1, number) :
                        size_.columns - first + 1;
                    for (auto column = first; column < (first + count); ++column) {
                        ClearCell(cursor_.row, column);
                        erased.emplace_back(cursor_.row, column);
                    }
                }
                continue;
            }
            const auto end = text.find('\x1b');
            const auto printed = text.substr(0, end);
            for (const auto &group : MeasureText<WidthPolicy::WindowsTerminalGraphemes>(printed)) {
                const auto width = [&] {
                    if ((group.text == "\u0600"sv) || (group.text == "\u0601"sv)) {
                        return 0;
                    }
                    if ((group.text == "日"sv) || (group.text == "日\u0600"sv) ||
                        (group.text == "日\u0601"sv) ||
                        (group.text == "本"sv) ||
                        (group.text == "👩‍💻"sv) || (group.text == "😀"sv)) {
                        return 2;
                    }
                    Require((group.text == "é"sv) || (group.text == "©"sv) ||
                        (group.text.size() == 1),
                        std::format("the frame fixture has no width for {:?}", group.text));
                    return 1;
                }();
                if (pending_wrap_ && (width != 0)) {
                    cursor_ = {.row = cursor_.row + 1, .column = 1};
                    pending_wrap_ = false;
                }
                Require((cursor_.row >= 1) && (cursor_.row <= size_.rows) &&
                    ((cursor_.column + width - 1) <= size_.columns),
                    "frame text escaped its fitted row"sv);
                for (auto offset = 0; offset < width; ++offset) {
                    ClearCell(cursor_.row, cursor_.column + offset);
                }
                for (auto offset = 0; offset < width; ++offset) {
                    cells_.insert_or_assign({cursor_.row, cursor_.column + offset},
                        Cell{std::string{group.text}, cursor_.column, width, reverse_});
                }
                painted.append(group.text);
                cursor_.column += width;
                if (cursor_.column > size_.columns) {
                    cursor_.column = size_.columns;
                    pending_wrap_ = true;
                }
            }
            text.remove_prefix(printed.size());
        }
    }

    [[nodiscard]] auto QueryCursor() noexcept -> std::optional<CursorPosition> {
        ++queries;
        return cursor_;
    }
    auto PresentFrame() noexcept -> void { ++presentations; }
    auto ResetActivity() noexcept -> void {
        painted.clear();
        erased.clear();
        presentations = 0;
        screen_erases = 0;
        writes = 0;
        bytes = 0;
        queries = 0;
        cursor_moves = 0;
        attribute_changes = 0;
    }
    auto Resize(const TerminalSize size, const std::optional<int> removed_top_rows = std::nullopt) -> void {
        const auto removed_rows = removed_top_rows.value_or(std::max(0, size_.rows - size.rows));
        auto retained = decltype(cells_){};
        for (auto &[position, cell] : cells_) {
            const auto row = position.first - removed_rows;
            if ((row >= 1) && (row <= size.rows) && (position.second <= size.columns)) {
                retained.emplace(std::pair{row, position.second}, std::move(cell));
            }
        }
        cells_ = std::move(retained);
        size_ = size;
    }
    [[nodiscard]] auto Row(const int row) const -> std::string {
        auto text = std::string{};
        for (auto column = 1; column <= size_.columns; ++column) {
            const auto found = cells_.find({row, column});
            if (found == cells_.end()) {
                text.push_back(' ');
            } else if (found->second.first_column == column) {
                text.append(found->second.text);
            }
        }
        while (!text.empty() && (text.back() == ' ')) {
            text.pop_back();
        }
        return text;
    }
    [[nodiscard]] auto AllReversed() const -> bool {
        return std::ranges::all_of(cells_, [](const auto &entry) {
            return entry.second.reverse;
        });
    }

    std::string painted;
    std::vector<std::pair<int, int>> erased;
    int presentations = 0;
    int screen_erases = 0;
    int writes = 0;
    std::size_t bytes = 0;
    int queries = 0;
    int cursor_moves = 0;
    int attribute_changes = 0;

private:
    auto ClearCell(const int row, const int column) -> void {
        if (const auto found = cells_.find({row, column}); found != cells_.end()) {
            const auto first = found->second.first_column;
            const auto width = found->second.width;
            for (auto offset = 0; offset < width; ++offset) {
                cells_.erase({row, first + offset});
            }
        }
    }
    TerminalSize size_;
    CursorPosition cursor_;
    std::map<std::pair<int, int>, Cell> cells_;
    bool reverse_ = false;
    bool pending_wrap_ = false;
};

class DrawingConsole : public FrameConsole {
public:
    using FrameConsole::FrameConsole;

    template <WidthPolicy Policy = WidthPolicy::AllModes>
    [[nodiscard]] auto KnownTextWidths(const std::string_view text) const {
        return presenter_.KnownTextWidths<Policy>(text);
    }

    template <WidthPolicy Policy = WidthPolicy::AllModes>
    [[nodiscard]] auto MeasureFrameLine(const FrameLine &line, const int row,
        const TerminalSize size) {
        return presenter_.MeasureLine<Policy>(*this, line, row, size);
    }

    auto InvalidateFrameRows(const int row, const int count) -> void {
        presenter_.InvalidateRows(row, count);
    }

    template <WidthPolicy Policy = WidthPolicy::AllModes>
    [[nodiscard]] auto Flip(const FrameBuffer &frame) -> bool {
        return presenter_.Flip<Policy>(*this, frame);
    }

private:
    DeltaFramePresenter presenter_;
};

}

export [[nodiscard]] auto RunFrameTests() -> bool {
    static_assert(FrameTerminal<FrameConsole>);
    auto passed = true;
    passed &= Test("adjacent Unicode groups sharing cursor positioning and highlighting"sv, [] {
        auto terminal = FrameConsole{{.rows = 2, .columns = 80}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 2, .columns = 80}};
        frame.rows.front().text = "日本 👩‍💻 é Installation";
        Require(presenter.Flip(terminal, frame), "the initial frame was not presented"sv);
        std::println("Initial frame: {} writes, {} bytes, {} cursor queries.",
            terminal.writes, terminal.bytes, terminal.queries);
        terminal.ResetActivity();
        frame.rows.front().reverse = true;
        Require(presenter.Flip(terminal, frame) && terminal.AllReversed(),
            "highlighting did not cover the Unicode row"sv);
        Require(terminal.Row(1) == frame.rows.front().text, "consecutive writes changed the text"sv);
        Require((terminal.writes == 1) && (terminal.queries == 0) &&
            (terminal.cursor_moves == 1) && (terminal.attribute_changes == 2),
            "adjacent cached groups produced redundant writes, queries, or control sequences"sv);
        std::println("Highlight change: {} write, {} bytes, {} cursor queries.",
            terminal.writes, terminal.bytes, terminal.queries);
    });
    passed &= Test("changing one digit beside Japanese, joined emoji and a combining accent"sv, [] {
        auto terminal = FrameConsole{{.rows = 2, .columns = 32}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 2, .columns = 32}};
        frame.rows.at(0).text = "日本 👩‍💻 é 1 end";
        Require(presenter.Flip(terminal, frame), "the initial frame was not presented"sv);
        terminal.ResetActivity();
        frame.rows.at(0).text = "日本 👩‍💻 é 2 end";
        Require(presenter.Flip(terminal, frame), "the changed digit was not presented"sv);
        Require((terminal.painted == "2") && terminal.erased.empty(),
            "changing a digit repainted or erased unchanged neighboring text"sv);
        Require(terminal.Row(1) == frame.rows.at(0).text, "the Unicode row was corrupted"sv);
        terminal.ResetActivity();
        Require(presenter.Flip(terminal, frame) && terminal.painted.empty() &&
            (terminal.presentations == 0), "an unchanged frame produced output"sv);
        frame.rows.at(0).reverse = true;
        Require(presenter.Flip(terminal, frame) && terminal.AllReversed(),
            "changing selection did not update every glyph's attributes"sv);
        Require(terminal.erased.empty() && (terminal.screen_erases == 0),
            "changing attributes erased existing glyphs"sv);
    });
    passed &= Test("shortening a Unicode row clearing displaced text from its blank tail"sv, [] {
        auto terminal = FrameConsole{{.rows = 2, .columns = 16}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 2, .columns = 16}};
        frame.rows.at(0).text = "日本 éXY";
        std::ignore = presenter.Flip(terminal, frame);
        // A console relay can place text beyond the end reported to the
        // application. The next shorter frame must leave that tail blank too.
        terminal.Write("\x1b[1;14Ht)"sv);
        terminal.ResetActivity();
        frame.rows.at(0).text = "日本";
        std::ignore = presenter.Flip(terminal, frame);
        Require(terminal.Row(1) == "日本", "removed text remained on screen"sv);
        Require(terminal.painted.empty() && !terminal.erased.empty() &&
            std::ranges::all_of(terminal.erased, [](const auto &cell) {
                return (cell.first == 1) && (cell.second > 4);
            }), "tail removal modified the retained Japanese glyphs"sv);
    });
    passed &= Test("replacing a wide glyph preserving a suffix at the same columns"sv, [] {
        auto terminal = FrameConsole{{.rows = 2, .columns = 16}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 2, .columns = 16}};
        frame.rows.at(0).text = "日xZ";
        std::ignore = presenter.Flip(terminal, frame);
        terminal.ResetActivity();
        frame.rows.at(0).text = "abxZ";
        std::ignore = presenter.Flip(terminal, frame);
        Require((terminal.Row(1) == "abxZ") && (terminal.painted == "ab"),
            "replacing a wide glyph damaged or repainted the positioned suffix"sv);
    });
    passed &= Test("a cached wide glyph at the last column and a later terminal resize"sv, [] {
        auto terminal = FrameConsole{{.rows = 2, .columns = 6}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 2, .columns = 6}};
        frame.rows.at(0).text = "😀";
        frame.rows.at(1).text = "日ab😀";
        std::ignore = presenter.Flip(terminal, frame);
        Require(terminal.Row(2) == "日ab😀", "the wide glyph did not fill the final two cells"sv);
        terminal.ResetActivity();
        frame.rows.at(1).text = "日abZ";
        std::ignore = presenter.Flip(terminal, frame);
        Require(terminal.Row(2) == "日abZ", "replacing the final glyph left a stale tail"sv);
        terminal.Resize({.rows = 3, .columns = 8});
        auto resized = FrameBuffer{TerminalSize{.rows = 3, .columns = 8}};
        resized.rows.at(0).text = "😀";
        resized.rows.at(1).text = "日abZ";
        resized.rows.at(2).text = "日本 é";
        std::ignore = presenter.Flip(terminal, resized);
        Require((terminal.Row(1) == "😀") && (terminal.Row(2) == "日abZ") &&
            (terminal.Row(3) == "日本 é"), "resizing corrupted the desired frame"sv);
    });
    passed &= Test("height reduction removing top rows and restoring the complete frame"sv, [] {
        auto terminal = FrameConsole{{.rows = 6, .columns = 24}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 6, .columns = 24}};
        frame.rows.at(0).text = "Header";
        frame.rows.at(1).text = "日本";
        frame.rows.at(3).text = "Old long entry";
        frame.rows.at(4).text = "Old other entry";
        frame.rows.at(5).text = "Footer";
        Require(presenter.Flip(terminal, frame), "the initial frame was not presented"sv);
        terminal.Resize({.rows = 3, .columns = 24});
        Require(terminal.Row(1) == "Old long entry",
            "the fixture did not reproduce removal of the top rows"sv);
        terminal.ResetActivity();
        auto resized = FrameBuffer{TerminalSize{.rows = 3, .columns = 24}};
        resized.rows.front().text = "Header";
        resized.rows.back().text = "Footer";
        Require(presenter.Flip(terminal, resized), "the shorter frame was not presented"sv);
        Require((terminal.Row(1) == "Header") && terminal.Row(2).empty() &&
            (terminal.Row(3) == "Footer"),
            "shrinking the terminal left shifted text or stale tails on screen"sv);
        Require(presenter.KnownTextWidths("日本"sv).has_value(),
            "height reduction discarded character-width measurements"sv);
        Require(terminal.screen_erases == 0,
            "height reduction cleared the screen before repainting"sv);
    });
    passed &= Test("height reduction restoring a message while preserving the blank rows below it"sv, [] {
        for (const auto removed_rows : {0, 1, 2, 3}) {
            auto terminal = FrameConsole{{.rows = 6, .columns = 24}};
            auto presenter = DeltaFramePresenter{};
            auto frame = FrameBuffer{TerminalSize{.rows = 6, .columns = 24}};
            frame.rows.front().text = "Enlarge the window.";
            Require(presenter.Flip(terminal, frame), "the initial message could not be displayed"sv);
            terminal.Resize({.rows = 3, .columns = 24}, removed_rows);
            terminal.ResetActivity();
            auto resized = FrameBuffer{TerminalSize{.rows = 3, .columns = 24}};
            resized.rows.front() = frame.rows.front();
            Require(presenter.Flip(terminal, resized) &&
                (terminal.Row(1) == "Enlarge the window."sv) &&
                terminal.Row(2).empty() && terminal.Row(3).empty(),
                std::format("removing {} top rows lost the message or left misplaced text", removed_rows));
            Require((terminal.writes == 1) && (terminal.queries == 0) &&
                (terminal.screen_erases == 0) &&
                std::ranges::all_of(terminal.erased, [](const auto &cell) { return cell.first == 1; }),
                "restoring the message repainted its blank rows or cleared the screen"sv);
        }
    });
    passed &= Test("height reduction repainting shifted text through the last occupied row"sv, [] {
        for (const auto removed_rows : {0, 1, 2, 3}) {
            auto terminal = FrameConsole{{.rows = 7, .columns = 24}};
            auto presenter = DeltaFramePresenter{};
            auto frame = FrameBuffer{TerminalSize{.rows = 7, .columns = 24}};
            frame.rows.front().text = "Heading";
            frame.rows.at(2).text = "Lower text";
            Require(presenter.Flip(terminal, frame), "the original text could not be displayed"sv);
            terminal.Resize({.rows = 4, .columns = 24}, removed_rows);
            terminal.ResetActivity();
            auto resized = FrameBuffer{TerminalSize{.rows = 4, .columns = 24}};
            resized.rows.front().text = "Heading";
            Require(presenter.Flip(terminal, resized) && (terminal.Row(1) == "Heading"sv) &&
                terminal.Row(2).empty() && terminal.Row(3).empty() && terminal.Row(4).empty(),
                std::format("removing {} top rows left shifted text in the blank area", removed_rows));
            Require(std::ranges::none_of(terminal.erased,
                    [](const auto &cell) { return cell.first == 4; }),
                "the unchanged blank final row was erased"sv);
        }
    });
    passed &= Test("a narrow ambiguous-width character beside the right margin"sv, [] {
        auto terminal = FrameConsole{{.rows = 2, .columns = 8}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 2, .columns = 8}};
        frame.rows.at(0).text = "abcdef©X";
        Require(presenter.Flip(terminal, frame) &&
            (terminal.Row(1) == frame.rows.at(0).text),
            "an ambiguous cursor report omitted the fitted row's final character"sv);
        terminal.ResetActivity();
        frame.rows.at(0).text = "abcdef©Y";
        Require(presenter.Flip(terminal, frame) && (terminal.painted == "Y"),
            "changing the last cell repainted the unchanged copyright character"sv);
    });
    passed &= Test("an unmeasured narrow final glyph replacing an old wide glyph"sv, [] {
        auto terminal = FrameConsole{{.rows = 2, .columns = 8}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 2, .columns = 8}};
        frame.rows.at(0).text = "abcdef日";
        std::ignore = presenter.Flip(terminal, frame);
        terminal.ResetActivity();
        frame.rows.at(0).text = "abcdef©";
        Require(presenter.Flip(terminal, frame) &&
            (terminal.Row(1) == frame.rows.at(0).text),
            "replacing an unmeasured final glyph damaged it or left the old tail"sv);
        Require(terminal.painted == "©", "the unchanged prefix was repainted"sv);
    });
    passed &= Test("changing and removing zero-width text after a full-width final glyph"sv, [] {
        auto terminal = FrameConsole{{.rows = 1, .columns = 8}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 1, .columns = 8}};
        frame.rows.front().text = "abcdef日\u0600";
        Require(presenter.Flip<WidthPolicy::WindowsTerminalGraphemes>(terminal, frame),
            "the fitted row could not be painted"sv);
        terminal.ResetActivity();
        frame.rows.front().text = "abcdef日\u0601";
        Require(presenter.Flip<WidthPolicy::WindowsTerminalGraphemes>(terminal, frame),
            "changing the zero-width suffix failed"sv);
        Require(terminal.Row(1) == frame.rows.front().text,
            "replacing a zero-width suffix erased the retained wide glyph"sv);
        Require(terminal.painted == "日\u0601",
            "replacing a zero-width suffix repainted the unchanged prefix"sv);
        frame.rows.front().text = "abcdef日";
        Require(presenter.Flip<WidthPolicy::WindowsTerminalGraphemes>(terminal, frame),
            "removing the zero-width suffix failed"sv);
        Require(terminal.Row(1) == "abcdef日", "tail erasure damaged the retained wide glyph"sv);
    });
    passed &= Test("frame line writes and explicit newlines remaining buffered until presentation"sv, [] {
        constexpr auto size = TerminalSize{.rows = 3, .columns = 12};
        auto terminal = DrawingConsole{size};
        auto frame = Frame{terminal, size};
        frame.WriteLine("abcdefghijklmno");
        frame.Write("\n");
        frame.WriteLine("second");
        frame.Write("\n");
        Require(frame.Ready() && (frame.CurrentRow() == 3),
            "explicit newlines did not position the next write on the third row"sv);
        Require((terminal.writes == 0) && (terminal.queries == 0),
            "line writes requested an endpoint that the following newline did not need"sv);
        Require(terminal.Flip(frame) && (terminal.Row(1) == "abcdefghi..."sv) &&
            (terminal.Row(2) == "second"sv) && terminal.Row(3).empty(),
            "the clipped and complete rows were not presented as prepared"sv);
    });
    passed &= Test("ordinary frame writes continuing from the current position and wrapping"sv, [] {
        constexpr auto size = TerminalSize{.rows = 3, .columns = 6};
        auto terminal = DrawingConsole{size};
        auto frame = Frame{terminal, size};
        Require(frame.Write("ab").stop == WrappingStop::EndOfText,
            "the initial prefix was not accepted"sv);
        Require((terminal.writes == 0) && (terminal.queries == 0),
            "a fitting initial write measured its unused endpoint"sv);
        const auto result = frame.Write("cdefghij");
        Require(frame.Ready() && result.remaining.text.empty() &&
            (result.stop == WrappingStop::EndOfText),
            "ordinary continuation did not accept all the text"sv);
        Require(terminal.Flip(frame) && (terminal.Row(1) == "abcdef"sv) &&
            (terminal.Row(2) == "ghij"sv),
            "ordinary continuation began at the wrong column or indented its wrap"sv);
    });
    passed &= Test("cached frame continuation indentation and a following explicit newline"sv, [] {
        constexpr auto size = TerminalSize{.rows = 3, .columns = 6};
        auto terminal = DrawingConsole{size};
        auto measured = FrameBuffer{size};
        measured.rows.at(0).text = "abcde";
        measured.rows.at(1).text = "fghij";
        Require(terminal.Flip(measured), "the fixture could not establish character widths"sv);
        terminal.ResetActivity();
        auto frame = Frame{terminal, size};
        frame.Write("ab");
        const auto result = frame.Write("cdefghij",
            FrameWriteOptions{.continuation_column = 3, .highlight = true});
        frame.Write("\n");
        frame.WriteLine("tail", FrameLineOptions{.highlight = true});
        Require(frame.Ready() && result.remaining.text.empty() &&
            (result.stop == WrappingStop::EndOfText) && (frame.CurrentRow() == 3),
            "indented continuation or its following newline lost the writing position"sv);
        Require((terminal.writes == 0) && (terminal.queries == 0),
            "cached continuation widths caused another terminal observation"sv);
        Require(terminal.Flip(frame) && (terminal.Row(1) == "abcdef"sv) &&
            (terminal.Row(2) == "  ghij"sv) && (terminal.Row(3) == "tail"sv),
            "the continuation indentation or subsequent row was incorrect"sv);
        Require(terminal.AllReversed(),
            "highlighting omitted a wrapped row or its indentation"sv);
    });
    passed &= Test("a clipped frame write preserving text before its current column"sv, [] {
        constexpr auto size = TerminalSize{.rows = 2, .columns = 10};
        auto terminal = DrawingConsole{size};
        auto frame = Frame{terminal, size};
        frame.WriteLine("123456789");
        frame.WriteLine("ABCD");
        frame.Write("\n");
        frame.WriteLine("after");
        Require(frame.Ready() && terminal.Flip(frame),
            "the clipped continuation could not be presented"sv);
        Require((terminal.Row(1) == "123456789."sv) && (terminal.Row(2) == "after"sv),
            "the clipped write's ellipsis replaced text preceding the write"sv);
    });
    passed &= Test("a complete Unicode group remaining intact in a frame write"sv, [] {
        constexpr auto size = TerminalSize{.rows = 2, .columns = 24};
        auto terminal = DrawingConsole{size};
        auto frame = Frame{terminal, size};
        const auto text = "日本 é 👩‍💻"sv;
        const auto result = frame.Write("{}", text);
        frame.Write("\n");
        Require(result.remaining.text.empty() && (result.stop == WrappingStop::EndOfText) &&
            (terminal.writes == 0) && (terminal.queries == 0),
            "fitting Unicode text was shortened or measured before presentation"sv);
        Require(terminal.Flip(frame) && (terminal.Row(1) == text),
            "a frame write altered a complete composed character"sv);
    });
    passed &= Test("a frame's filled final row returning the unwritten suffix without a redraw failure"sv, [] {
        constexpr auto size = TerminalSize{.rows = 1, .columns = 4};
        auto terminal = DrawingConsole{size};
        auto frame = Frame{terminal, size};
        frame.Write("abcd");
        const auto result = frame.Write("e");
        Require(frame.Ready() && (result.stop == WrappingStop::RowLimit) &&
            (result.remaining.text == "e"sv),
            "ordinary row exhaustion was reported as a failed layout or consumed the suffix"sv);
        Require(terminal.Flip(frame) && (terminal.Row(1) == "abcd"sv),
            "writing after a full final row damaged the retained text"sv);
    });
    passed &= Test("copying a header frame and replacing later rows through drawing operations"sv, [] {
        constexpr auto size = TerminalSize{.rows = 3, .columns = 20};
        auto terminal = DrawingConsole{size};
        auto header = Frame{terminal, size};
        header.WriteLine("Header");
        header.Write("\n");
        header.WriteLine("discarded");
        auto frame = header;
        frame.MoveTo({2, 1});
        frame.ClearFromCurrentRow();
        frame.WriteLine("replacement");
        Require((terminal.writes == 0) && (terminal.queries == 0),
            "copying and replacing whole rows required terminal I/O"sv);
        Require(terminal.Flip(frame) && (terminal.Row(1) == "Header"sv) &&
            (terminal.Row(2) == "replacement"sv) && terminal.Row(3).empty(),
            "replacing later rows changed the header or retained discarded text"sv);
        Require(terminal.Flip(header) && (terminal.Row(2) == "discarded"sv),
            "editing the copied frame changed the original frame"sv);
    });
    passed &= Test("format-string newlines creating rows while argument newlines remain visible text"sv, [] {
        constexpr auto size = TerminalSize{.rows = 4, .columns = 32};
        auto terminal = DrawingConsole{size};
        auto frame = Frame{terminal, size};
        const auto result = frame.Write("First\n{}\n{}", "second\nline", '\n');
        frame.Write("\n");
        frame.WriteLine("{}", std::string{"fourth\nline"});
        Require(frame.Ready() && result.remaining.text.empty() &&
            (result.stop == WrappingStop::EndOfText) && (frame.CurrentRow() == 4),
            "supplied newlines moved the writing position or stopped the formatted output"sv);
        Require(terminal.Flip(frame) && (terminal.Row(1) == "First"sv) &&
            (terminal.Row(2) == "second\\nline"sv) && (terminal.Row(3) == "\\n"sv) &&
            (terminal.Row(4) == "fourth\\nline"sv),
            "format-string line feeds and argument line feeds were not distinguished on screen"sv);
    });
    passed &= Test("string objects, views, pointers, and arrays sharing preparation before frame formatting"sv, [] {
        constexpr auto size = TerminalSize{.rows = 2, .columns = 80};
        auto terminal = DrawingConsole{size};
        auto frame = Frame{terminal, size};
        const auto string = std::string{"string\nvalue"};
        constexpr auto view = "view\nvalue"sv;
        constexpr auto pointer = "pointer\nvalue";
        constexpr auto &array = "array\nvalue";
        frame.WriteLine("{}|{}|{}|{}", string, view, pointer, array);
        frame.Write("\n");
        frame.WriteLine("{}", "before\x1b[2Jafter\x1b]0;title\x07" "end");
        Require(terminal.Flip(frame) &&
            (terminal.Row(1) == "string\\nvalue|view\\nvalue|pointer\\nvalue|array\\nvalue"sv) &&
            (terminal.Row(2) == "beforeafterend"sv),
            "an argument representation bypassed preparation or a supplied VT command affected the frame"sv);
    });
    passed &= Test("frame formatting applying numeric formats and string alignment and precision"sv, [] {
        constexpr auto size = TerminalSize{.rows = 2, .columns = 32};
        auto terminal = DrawingConsole{size};
        auto frame = Frame{terminal, size};
        frame.WriteLine("|{:>8.4}|{:04}|{:.2f}|", "alphabet", 7, 2.5);
        frame.Write("\n");
        frame.WriteLine("|{:>4}|{:.3}|", '\n', "a\nb");
        Require(terminal.Flip(frame) && (terminal.Row(1) == "|    alph|0007|2.50|"sv) &&
            (terminal.Row(2) == "|  \\n|a\\n|"sv),
            "formatting did not apply numeric formats or string specifications after preparation"sv);
    });
    passed &= Test("an unwritten formatted suffix owning its text and resuming without double preparation"sv, [] {
        constexpr auto size = TerminalSize{.rows = 1, .columns = 6};
        auto terminal = DrawingConsole{size};
        const auto first = [&terminal, size] {
            auto frame = Frame{terminal, size};
            const auto result = frame.Write("{}", std::string{"prefix\nrest"});
            Require(frame.Ready() && (result.stop == WrappingStop::RowLimit) &&
                terminal.Flip(frame) && (terminal.Row(1) == "prefix"sv),
                "the initial page did not stop after its fitting prefix"sv);
            return result;
        }();
        Require(first.remaining.text == "\\nrest"sv,
            "the returned suffix did not survive the formatted text and initial frame"sv);
        auto frame = Frame{terminal, size};
        const auto resumed = frame.Write("{}", first.remaining);
        Require(frame.Ready() && resumed.remaining.text.empty() &&
            (resumed.stop == WrappingStop::EndOfText) && terminal.Flip(frame) &&
            (terminal.Row(1) == "\\nrest"sv),
            "resuming prepared output escaped its visible control notation a second time"sv);
    });
    passed &= Test("a line write's explicit newline positioning subsequent text at column one"sv, [] {
        constexpr auto size = TerminalSize{.rows = 3, .columns = 16};
        auto terminal = DrawingConsole{size};
        auto frame = Frame{terminal, size};
        frame.WriteLine("hello\n");
        frame.WriteLine("world");
        frame.Write("!");
        Require(frame.Ready() && (frame.CurrentRow() == 2) && terminal.Flip(frame) &&
            (terminal.Row(1) == "hello"sv) && (terminal.Row(2) == "world!"sv) &&
            terminal.Row(3).empty(),
            "WriteLine misplaced an explicit newline or added an implicit newline before appended text"sv);
    });
    passed &= Test("each explicit line being clipped independently instead of wrapping its overflow"sv, [] {
        constexpr auto size = TerminalSize{.rows = 3, .columns = 8};
        auto terminal = DrawingConsole{size};
        auto frame = Frame{terminal, size};
        const auto result = frame.WriteLine("abcdefghijklm\n1234567890\nlast");
        Require(frame.Ready() && result.remaining.text.empty() &&
            (result.stop == WrappingStop::EndOfText) && terminal.Flip(frame) &&
            (terminal.Row(1) == "abcde..."sv) && (terminal.Row(2) == "12345..."sv) &&
            (terminal.Row(3) == "last"sv),
            "clipped overflow was returned as unprocessed text or occupied a later logical line"sv);
    });
    passed &= Test("leading, consecutive, and trailing newlines preserving blank rows in line writes"sv, [] {
        constexpr auto size = TerminalSize{.rows = 6, .columns = 16};
        auto terminal = DrawingConsole{size};
        auto frame = Frame{terminal, size};
        frame.WriteLine("\nAlpha\n\nBeta\n");
        frame.WriteLine("tail");
        Require(frame.Ready() && (frame.CurrentRow() == 5) && terminal.Flip(frame) &&
            terminal.Row(1).empty() && (terminal.Row(2) == "Alpha"sv) &&
            terminal.Row(3).empty() && (terminal.Row(4) == "Beta"sv) &&
            (terminal.Row(5) == "tail"sv) && terminal.Row(6).empty(),
            "a leading, consecutive, or trailing newline was lost or an implicit newline was added"sv);
    });
    passed &= Test("line writes distinguishing format newlines from string and character data"sv, [] {
        constexpr auto size = TerminalSize{.rows = 3, .columns = 24};
        auto terminal = DrawingConsole{size};
        auto frame = Frame{terminal, size};
        frame.WriteLine("{}\n{}", "top\nvalue", '\n');
        Require(frame.Ready() && (frame.CurrentRow() == 2) && terminal.Flip(frame) &&
            (terminal.Row(1) == "top\\nvalue"sv) && (terminal.Row(2) == "\\n"sv) &&
            terminal.Row(3).empty(),
            "argument data changed the row layout or the format string's newline was displayed as text"sv);
    });
    passed &= Test("trailing frame options applying highlighting and clipping after formatted arguments"sv, [] {
        constexpr auto size = TerminalSize{.rows = 2, .columns = 12};
        auto terminal = DrawingConsole{size};
        auto frame = Frame{terminal, size};
        constexpr auto options = FrameLineOptions{
            .clipping = FrameClipping::Ellipsis, .highlight = true};
        frame.WriteLine("{}", PreparedText{.text = "name\\n"}, options);
        frame.Write("\n");
        frame.Write("{} {}", "next", 7, FrameWriteOptions{.highlight = true});
        Require(frame.Ready() && terminal.Flip(frame) && terminal.AllReversed() &&
            (terminal.Row(1) == "name\\n..."sv) && (terminal.Row(2) == "next 7"sv),
            "trailing options were formatted as data or failed to apply the requested row policy"sv);
    });
    passed &= Test("line writes returning later unprocessed lines while discarding clipped overflow"sv, [] {
        constexpr auto size = TerminalSize{.rows = 1, .columns = 8};
        auto terminal = DrawingConsole{size};
        auto frame = Frame{terminal, size};
        const auto first = frame.WriteLine("abcdefghijk\n\n{}", "last\nvalue");
        Require(frame.Ready() && (first.stop == WrappingStop::RowLimit) && (first.rows == 1) &&
            (first.remaining.text == "\nlast\\nvalue"sv) && terminal.Flip(frame) &&
            (terminal.Row(1) == "abcde..."sv),
            "the remainder lost an unprocessed blank line or retained deliberately clipped overflow"sv);
        const auto subsequent = frame.WriteLine("still waiting");
        Require((subsequent.stop == WrappingStop::RowLimit) && (subsequent.rows == 0) &&
            (subsequent.remaining.text == "still waiting"sv),
            "a line write starting below the frame consumed its input or reported completion"sv);
        constexpr auto resumed_size = TerminalSize{.rows = 2, .columns = 16};
        terminal.Resize(resumed_size);
        auto resumed = Frame{terminal, resumed_size};
        const auto last = resumed.WriteLine("{}", first.remaining);
        Require(last.remaining.text.empty() && (last.stop == WrappingStop::EndOfText) &&
            terminal.Flip(resumed) && terminal.Row(1).empty() &&
            (terminal.Row(2) == "last\\nvalue"sv),
            "resuming the line remainder lost the blank row or prepared its data twice"sv);
    });
    return passed;
}
